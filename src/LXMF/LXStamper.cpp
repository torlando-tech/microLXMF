#include "LXStamper.h"
#include "Type.h"
#include <Identity.h>
#include <Cryptography/HKDF.h>
#include <Cryptography/Random.h>
#include <RNG.h>
#include <Utilities/OS.h>
#include <Log.h>

#include <SHA256.h>

#ifdef ESP_PLATFORM
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include "esp_task_wdt.h"
#endif

#include <atomic>
#include <mutex>

using namespace LXMF;
using namespace RNS;

// =============================================================================
// Async stamp state — single global slot. Only one stamp in flight at a time.
// =============================================================================
namespace {
	enum class AsyncState : uint8_t {
		IDLE = 0,
		RUNNING = 1,
		DONE_SUCCESS = 2,
		DONE_FAIL = 3,
	};

	struct AsyncStampSlot {
		std::atomic<AsyncState> state{AsyncState::IDLE};
		// Inputs (read by worker, set by main thread before spawn).
		Bytes message_id;
		uint8_t stamp_cost{0};
		uint16_t expand_rounds{0};
		// Outputs (written by worker, read by main thread after DONE).
		Bytes result_stamp;
		uint8_t result_value{0};
		// Mutex protects the input/output Bytes from racy access during
		// the brief windows where state is transitioning. The atomic
		// state member coordinates which side may touch what.
		std::mutex slot_mutex;
	};

	static AsyncStampSlot g_async_stamp;
}

// Count leading zero bits in a hash buffer
static uint8_t count_leading_zeros(const uint8_t* hash, size_t size) {
	uint8_t value = 0;
	for (size_t i = 0; i < size && value < 256; i++) {
		uint8_t byte = hash[i];
		if (byte == 0) {
			value += 8;
		} else {
			while ((byte & 0x80) == 0 && value < 256) {
				value++;
				byte <<= 1;
			}
			break;
		}
	}
	return value;
}

// Pack uint16 as msgpack (fixint or uint16 format)
Bytes LXStamper::msgpack_pack_uint16(uint16_t n) {
	Bytes result;
	if (n <= 127) {
		// Positive fixint: single byte 0x00-0x7f
		result.append((uint8_t)n);
	} else if (n <= 255) {
		// uint8: 0xcc followed by uint8
		result.append((uint8_t)0xcc);
		result.append((uint8_t)n);
	} else {
		// uint16: 0xcd followed by big-endian uint16
		result.append((uint8_t)0xcd);
		result.append((uint8_t)(n >> 8));
		result.append((uint8_t)(n & 0xff));
	}
	return result;
}

// Generate workblock from message ID using HKDF expansion
Bytes LXStamper::stamp_workblock(const Bytes& material, uint16_t expand_rounds) {
	DEBUG("Generating stamp workblock with " + std::to_string(expand_rounds) + " rounds");

	// Pre-allocate for efficiency
	// Each round produces 256 bytes
	size_t total_size = 256 * expand_rounds;
	Bytes workblock;
	workblock.reserve(total_size);

	for (uint16_t n = 0; n < expand_rounds; n++) {
		// Pack n with msgpack (matches Python: msgpack.packb(n))
		Bytes packed_n = msgpack_pack_uint16(n);

		// salt = full_hash(material + msgpack.packb(n))
		Bytes salt_input;
		salt_input << material << packed_n;
		Bytes salt = Identity::full_hash(salt_input);

		// chunk = hkdf(length=256, derive_from=material, salt=salt, context=None)
		Bytes chunk = Cryptography::hkdf(256, material, salt, {});

		workblock << chunk;
	}

	DEBUG("Workblock generated: " + std::to_string(workblock.size()) + " bytes");
	return workblock;
}

// Count leading zero bits in SHA256(workblock + stamp)
uint8_t LXStamper::stamp_value(const Bytes& workblock, const Bytes& stamp) {
	// Hash the concatenation
	Bytes material;
	material << workblock << stamp;
	Bytes hash = Identity::full_hash(material);

	// Count leading zero bits
	uint8_t value = 0;

	for (size_t i = 0; i < hash.size() && value < 256; i++) {
		uint8_t byte = hash.data()[i];
		if (byte == 0) {
			// Entire byte is zeros
			value += 8;
		} else {
			// Count leading zeros in this byte
			while ((byte & 0x80) == 0 && value < 256) {
				value++;
				byte <<= 1;
			}
			break;  // Non-zero bit found
		}
	}

	return value;
}

// Check if stamp meets target cost
bool LXStamper::stamp_valid(const Bytes& stamp, uint8_t target_cost, const Bytes& workblock) {
	if (stamp.size() != STAMP_SIZE) {
		return false;
	}
	return stamp_value(workblock, stamp) >= target_cost;
}

// Generate a valid stamp (blocking, CPU-intensive)
// OPTIMIZED: Pre-hash workblock once, only hash 16-byte stamp per iteration
// This is ~16,000x faster than hashing the full 256KB workblock each time
std::pair<Bytes, uint8_t> LXStamper::generate_stamp(
	const Bytes& message_id,
	uint8_t stamp_cost,
	uint16_t expand_rounds,
	std::atomic<bool>* cancel,
	ProgressCallback progress)
{
	INFO("Generating stamp with cost " + std::to_string(stamp_cost) + " for " + message_id.toHex());

	// Generate workblock
	Bytes workblock = stamp_workblock(message_id, expand_rounds);

	// OPTIMIZATION: Pre-hash the workblock once and save the SHA256 state
	// This avoids re-hashing 256KB for every stamp attempt
	SHA256 base_hash;
	base_hash.reset();
	base_hash.update(workblock.data(), workblock.size());

	uint32_t rounds = 0;
	double start_time = Utilities::OS::time();
	uint8_t stamp_buffer[STAMP_SIZE];
	uint8_t hash_result[32];

	while (true) {
		// Check for cancellation
		if (cancel && cancel->load()) {
			INFO("Stamp generation cancelled after " + std::to_string(rounds) + " rounds");
			return {{}, 0};
		}

		// Generate random stamp candidate DIRECTLY into the stack
		// buffer. Going through Cryptography::random() returns a Bytes
		// object whose internal buffer is heap-allocated — and on
		// pyxis the global operator new is overridden to ps_malloc()
		// (RNS_PSRAM_ALLOCATOR). At cost=16 we run ~65k iterations and
		// the per-iteration PSRAM alloc/free pair dominates: stamps
		// that should take seconds were taking 4+ minutes. Direct
		// RNG fill keeps the hot path entirely in internal SRAM.
		RNG.rand(stamp_buffer, STAMP_SIZE);
		rounds++;

		// OPTIMIZATION: Copy the pre-computed hash state and only hash the stamp
		SHA256 test_hash = base_hash;  // Copy constructor copies state
		test_hash.update(stamp_buffer, STAMP_SIZE);
		test_hash.finalize(hash_result, 32);

		// Count leading zeros in the hash
		uint8_t value = count_leading_zeros(hash_result, 32);

		// Check if it meets the target cost
		if (value >= stamp_cost) {
			double duration = Utilities::OS::time() - start_time;
			double speed = (duration > 0) ? (rounds / duration) : 0;

			INFO("Stamp with value " + std::to_string(value) + " generated in " +
				 std::to_string((int)duration) + "s, " + std::to_string(rounds) +
				 " rounds, " + std::to_string((int)speed) + " rounds/sec");

			// Wrap the winning stamp into a Bytes for the return —
			// only one PSRAM alloc, not 65k.
			return {Bytes(stamp_buffer, STAMP_SIZE), value};
		}

		// Progress callback every 1000 rounds
		if (progress && (rounds % 1000 == 0)) {
			progress(rounds);
		}

		// Log progress every 5000 rounds
		if (rounds % 5000 == 0) {
			double elapsed = Utilities::OS::time() - start_time;
			double speed = (elapsed > 0) ? (rounds / elapsed) : 0;
			DEBUG("Stamp generation: " + std::to_string(rounds) + " rounds, " +
				  std::to_string((int)speed) + " rounds/sec");
		}

		// Yield to allow other tasks (LVGL, network) to run
		// This prevents UI freeze during stamp generation
		// Yield every 10 rounds (was 100) for better UI responsiveness
#ifdef ESP_PLATFORM
		if (rounds % 10 == 0) {
			vTaskDelay(1);        // Yield for 1 tick
			esp_task_wdt_reset(); // Feed watchdog during long operations
		}
#endif
	}

	// Should never reach here
	return {{}, 0};
}

// Validate propagation node stamp
std::tuple<Bytes, Bytes, uint8_t, Bytes> LXStamper::validate_pn_stamp(
	const Bytes& transient_data,
	uint8_t target_cost)
{
	// Check minimum size: need at least LXMF_OVERHEAD + STAMP_SIZE
	if (transient_data.size() <= Type::Constants::LXMF_OVERHEAD + STAMP_SIZE) {
		WARNING("Transient data too short for stamp validation");
		return {{}, {}, 0, {}};
	}

	// Extract lxm_data and stamp
	size_t lxm_data_len = transient_data.size() - STAMP_SIZE;
	Bytes lxm_data = transient_data.left(lxm_data_len);
	Bytes stamp = transient_data.mid(lxm_data_len, STAMP_SIZE);

	// Calculate transient_id = full_hash(lxm_data)
	Bytes transient_id = Identity::full_hash(lxm_data);

	// Generate workblock with PN-specific rounds
	Bytes workblock = stamp_workblock(transient_id, WORKBLOCK_EXPAND_ROUNDS_PN);

	// Validate stamp
	if (!stamp_valid(stamp, target_cost, workblock)) {
		DEBUG("PN stamp validation failed for transient_id " + transient_id.toHex());
		return {{}, {}, 0, {}};
	}

	uint8_t value = stamp_value(workblock, stamp);
	DEBUG("PN stamp validated: transient_id=" + transient_id.toHex() +
		  ", value=" + std::to_string(value));

	return {transient_id, lxm_data, value, stamp};
}

// =============================================================================
// Async stamp generation — moves the cost=16 grind off the main loop.
// =============================================================================

#ifdef ESP_PLATFORM
// Worker task body. Self-deletes on completion. Reads inputs from
// the global slot, writes outputs back, transitions state to DONE_*.
extern "C" {
static void stamp_worker_task(void* arg) {
	(void)arg;
	Bytes message_id;
	uint8_t stamp_cost;
	uint16_t expand_rounds;
	{
		std::lock_guard<std::mutex> lk(g_async_stamp.slot_mutex);
		message_id = g_async_stamp.message_id;
		stamp_cost = g_async_stamp.stamp_cost;
		expand_rounds = g_async_stamp.expand_rounds;
	}
	auto [stamp, value] = LXStamper::generate_stamp(
		message_id, stamp_cost, expand_rounds);
	{
		std::lock_guard<std::mutex> lk(g_async_stamp.slot_mutex);
		if (stamp.size() == LXStamper::STAMP_SIZE) {
			g_async_stamp.result_stamp = stamp;
			g_async_stamp.result_value = value;
			g_async_stamp.state.store(AsyncState::DONE_SUCCESS,
			                          std::memory_order_release);
		} else {
			g_async_stamp.state.store(AsyncState::DONE_FAIL,
			                          std::memory_order_release);
		}
	}
	vTaskDelete(nullptr);
}
}  // extern "C"
#endif

bool LXStamper::start_async(
	const Bytes& message_id,
	uint8_t stamp_cost,
	uint16_t expand_rounds)
{
	// Refuse if a stamp is already in flight or its result is still
	// waiting to be consumed. Caller must take_async_result() first.
	AsyncState st = g_async_stamp.state.load(std::memory_order_acquire);
	if (st != AsyncState::IDLE) {
		DEBUG("LXStamper::start_async: refusing — state is " +
		      std::to_string((int)st));
		return false;
	}
	{
		std::lock_guard<std::mutex> lk(g_async_stamp.slot_mutex);
		g_async_stamp.message_id = message_id;
		g_async_stamp.stamp_cost = stamp_cost;
		g_async_stamp.expand_rounds = expand_rounds;
		g_async_stamp.result_stamp = Bytes();  // clear previous
		g_async_stamp.result_value = 0;
	}
	g_async_stamp.state.store(AsyncState::RUNNING, std::memory_order_release);

#ifdef ESP_PLATFORM
	// Stamp worker needs ~12KB stack: workblock pointer (256KB on
	// PSRAM heap, not stack), SHA256 state (~104B), small temp buffers.
	// Run at priority 1 (idle is 0, loopTask is also 1) so it doesn't
	// preempt LVGL or higher-priority tasks. Use core 0 to leave core
	// 1 (the Arduino loopTask core) responsive to UI/serial.
	BaseType_t ok = xTaskCreatePinnedToCore(
		stamp_worker_task,
		"lxstamp",
		12 * 1024,
		nullptr,
		1,
		nullptr,
		0);
	if (ok != pdPASS) {
		ERROR("LXStamper::start_async: xTaskCreate failed");
		g_async_stamp.state.store(AsyncState::IDLE, std::memory_order_release);
		return false;
	}
	INFO("LXStamper::start_async: worker spawned for cost " +
	     std::to_string(stamp_cost));
	return true;
#else
	// Native test build — run synchronously, then transition to DONE.
	auto [stamp, value] = generate_stamp(message_id, stamp_cost, expand_rounds);
	{
		std::lock_guard<std::mutex> lk(g_async_stamp.slot_mutex);
		if (stamp.size() == STAMP_SIZE) {
			g_async_stamp.result_stamp = stamp;
			g_async_stamp.result_value = value;
			g_async_stamp.state.store(AsyncState::DONE_SUCCESS,
			                          std::memory_order_release);
		} else {
			g_async_stamp.state.store(AsyncState::DONE_FAIL,
			                          std::memory_order_release);
		}
	}
	return true;
#endif
}

bool LXStamper::is_async_running() {
	return g_async_stamp.state.load(std::memory_order_acquire)
	       == AsyncState::RUNNING;
}

bool LXStamper::is_async_done() {
	AsyncState st = g_async_stamp.state.load(std::memory_order_acquire);
	return st == AsyncState::DONE_SUCCESS || st == AsyncState::DONE_FAIL;
}

std::pair<Bytes, uint8_t> LXStamper::take_async_result() {
	AsyncState st = g_async_stamp.state.load(std::memory_order_acquire);
	if (st != AsyncState::DONE_SUCCESS && st != AsyncState::DONE_FAIL) {
		// Caller polled too early or lost track — return empty.
		return {{}, 0};
	}
	std::pair<Bytes, uint8_t> out;
	{
		std::lock_guard<std::mutex> lk(g_async_stamp.slot_mutex);
		if (st == AsyncState::DONE_SUCCESS) {
			out = {g_async_stamp.result_stamp, g_async_stamp.result_value};
		} else {
			out = {{}, 0};
		}
		// Clear inputs/outputs and reset state to IDLE so a new
		// stamp can be started.
		g_async_stamp.result_stamp = Bytes();
		g_async_stamp.result_value = 0;
		g_async_stamp.message_id = Bytes();
	}
	g_async_stamp.state.store(AsyncState::IDLE, std::memory_order_release);
	return out;
}
