#include "MessageStore.h"
#include <microReticulum/Log.h>
#include <microReticulum/Utilities/OS.h>

#include <ArduinoJson.h>
#include <algorithm>
#include <sstream>

using namespace LXMF;
using namespace RNS;

// ConversationInfo helper methods
bool MessageStore::ConversationInfo::add_message_hash(const Bytes& hash) {
	// Check if already exists
	if (has_message(hash)) {
		return false;
	}
	// Check if pool is full
	if (message_count >= MAX_MESSAGES_PER_CONVERSATION) {
		return false;
	}
	// Copy hash to fixed array
	size_t len = std::min(hash.size(), MESSAGE_HASH_SIZE);
	memcpy(message_hashes[message_count], hash.data(), len);
	if (len < MESSAGE_HASH_SIZE) {
		memset(message_hashes[message_count] + len, 0, MESSAGE_HASH_SIZE - len);
	}
	++message_count;
	return true;
}

bool MessageStore::ConversationInfo::has_message(const Bytes& hash) const {
	if (hash.size() == 0 || hash.size() > MESSAGE_HASH_SIZE) return false;
	for (size_t i = 0; i < message_count; ++i) {
		if (memcmp(message_hashes[i], hash.data(), hash.size()) == 0) {
			return true;
		}
	}
	return false;
}

bool MessageStore::ConversationInfo::remove_message_hash(const Bytes& hash) {
	if (hash.size() == 0 || hash.size() > MESSAGE_HASH_SIZE) return false;
	for (size_t i = 0; i < message_count; ++i) {
		if (memcmp(message_hashes[i], hash.data(), hash.size()) == 0) {
			// Shift remaining elements down
			for (size_t j = i; j < message_count - 1; ++j) {
				memcpy(message_hashes[j], message_hashes[j + 1], MESSAGE_HASH_SIZE);
			}
			memset(message_hashes[message_count - 1], 0, MESSAGE_HASH_SIZE);
			--message_count;
			return true;
		}
	}
	return false;
}

void MessageStore::ConversationInfo::clear() {
	memset(peer_hash, 0, PEER_HASH_SIZE);
	memset(message_hashes, 0, sizeof(message_hashes));
	message_count = 0;
	last_activity = 0.0;
	unread_count = 0;
	memset(last_message_hash, 0, MESSAGE_HASH_SIZE);
	memset(display_name, 0, sizeof(display_name));
}

// ConversationSlot helper method
void MessageStore::ConversationSlot::clear() {
	in_use = false;
	memset(peer_hash, 0, PEER_HASH_SIZE);
	info.clear();
}

// Constructor
MessageStore::MessageStore(const std::string& base_path) :
	_base_path(base_path),
	_initialized(false)
{
	INFO("Initializing MessageStore at: " + _base_path);

	// Initialize pool
	for (size_t i = 0; i < MAX_CONVERSATIONS; ++i) {
		_conversations_pool[i].clear();
	}

	if (initialize_storage()) {
		load_index();
		_initialized = true;
		INFO("MessageStore initialized with " + std::to_string(count_conversations()) + " conversations");
	} else {
		ERROR("Failed to initialize MessageStore");
	}
}

MessageStore::~MessageStore() {
	if (_initialized) {
		save_index();
	}
	TRACE("MessageStore destroyed");
}

// Initialize storage directories
bool MessageStore::initialize_storage() {
	// Create short directories for SPIFFS compatibility
	// SPIFFS is flat so these are mostly no-ops, but we try anyway
	Utilities::OS::create_directory("/m");  // messages
	Utilities::OS::create_directory("/c");  // conversations

	DEBUG("Storage directories initialized");
	return true;
}

// Load conversation index from disk
void MessageStore::load_index() {
	std::string index_path = "/conv.json";  // Short path for SPIFFS

	if (!Utilities::OS::file_exists(index_path.c_str())) {
		DEBUG("No existing conversation index found");
		return;
	}

	try {
		// Read JSON file via OS abstraction (SPIFFS compatible)
		Bytes data;
		if (Utilities::OS::read_file(index_path.c_str(), data) == 0) {
			WARNING("Failed to read index file or empty: " + index_path);
			return;
		}

		// Parse JSON from bytes using reusable document to reduce heap fragmentation
		_json_doc.clear();
		DeserializationError error = deserializeJson(_json_doc, data.data(), data.size());

		if (error) {
			ERROR("Failed to parse conversation index: " + std::string(error.c_str()));
			return;
		}

		// Load conversations into pool
		JsonArray conversations = _json_doc["conversations"].as<JsonArray>();
		size_t slot_index = 0;
		for (JsonObject conv : conversations) {
			if (slot_index >= MAX_CONVERSATIONS) {
				WARNING("Too many conversations in index, some will be skipped");
				break;
			}

			ConversationSlot& slot = _conversations_pool[slot_index];
			slot.in_use = true;

			// Parse peer hash
			const char* peer_hex = conv["peer_hash"];
			Bytes peer_bytes;
			peer_bytes.assignHex(peer_hex);
			slot.set_peer_hash(peer_bytes);
			slot.info.set_peer_hash(peer_bytes);

			// Parse message hashes
			JsonArray messages = conv["messages"].as<JsonArray>();
			for (const char* msg_hex : messages) {
				if (slot.info.message_count >= MAX_MESSAGES_PER_CONVERSATION) {
					WARNING("Too many messages in conversation, some will be skipped");
					break;
				}
				Bytes msg_hash;
				msg_hash.assignHex(msg_hex);
				slot.info.add_message_hash(msg_hash);
			}

			// Parse metadata
			slot.info.last_activity = conv["last_activity"] | 0.0;
			slot.info.unread_count = conv["unread_count"] | 0;

			if (!conv["last_message_hash"].isNull()) {
				const char* last_msg_hex = conv["last_message_hash"];
				Bytes last_msg_bytes;
				last_msg_bytes.assignHex(last_msg_hex);
				slot.info.set_last_message_hash(last_msg_bytes);
			}

			// Restore the cached display name. Authoritative source is
			// Identity::recall_app_data, which is in-memory and lost on
			// reboot; we cache the last-seen name here so the conv list
			// can show real names instead of hashes immediately on cold
			// start. The cache gets refreshed once a fresh announce
			// arrives.
			if (!conv["display_name"].isNull()) {
				const char* dn = conv["display_name"];
				if (dn) {
					strncpy(slot.info.display_name, dn, MAX_DISPLAY_NAME_LEN);
					slot.info.display_name[MAX_DISPLAY_NAME_LEN] = '\0';
				}
			}

			++slot_index;
		}

		DEBUG("Loaded " + std::to_string(count_conversations()) + " conversations from index");

	} catch (const std::exception& e) {
		ERROR("Exception loading conversation index: " + std::string(e.what()));
	}
}

// Save conversation index to disk
bool MessageStore::save_index() {
	std::string index_path = "/conv.json";  // Short path for SPIFFS

	try {
		// Use reusable document to reduce heap fragmentation
		_json_doc.clear();
		JsonArray conversations = _json_doc["conversations"].to<JsonArray>();

		// Serialize each active conversation from pool
		for (size_t i = 0; i < MAX_CONVERSATIONS; ++i) {
			const ConversationSlot& slot = _conversations_pool[i];
			if (!slot.in_use) {
				continue;
			}

			const ConversationInfo& info = slot.info;

			JsonObject conv = conversations.add<JsonObject>();
			conv["peer_hash"] = slot.peer_hash_bytes().toHex();
			conv["last_activity"] = info.last_activity;
			conv["unread_count"] = info.unread_count;

			Bytes last_msg = info.last_message_hash_bytes();
			if (last_msg) {
				conv["last_message_hash"] = last_msg.toHex();
			}

			// Persist the cached display name (if any) so it survives
			// reboots — see load_index for the rationale.
			if (info.display_name[0] != '\0') {
				conv["display_name"] = info.display_name;
			}

			// Serialize message hashes
			JsonArray messages = conv["messages"].to<JsonArray>();
			for (size_t j = 0; j < info.message_count; ++j) {
				messages.add(info.message_hash_bytes(j).toHex());
			}
		}

		// Serialize to string then write via OS abstraction (SPIFFS compatible)
		std::string json_str;
		serializeJsonPretty(_json_doc, json_str);
		Bytes data((const uint8_t*)json_str.data(), json_str.size());

		if (Utilities::OS::write_file(index_path.c_str(), data) != data.size()) {
			ERROR("Failed to write index file: " + index_path);
			return false;
		}

		DEBUG("Saved conversation index");
		return true;

	} catch (const std::exception& e) {
		ERROR("Exception saving conversation index: " + std::string(e.what()));
		return false;
	}
}

// Save message to storage
bool MessageStore::save_message(const LXMessage& message) {
	if (!_initialized) {
		ERROR("MessageStore not initialized");
		return false;
	}

	INFO("Saving message: " + message.hash().toHex());

	try {
		// Use reusable document to reduce heap fragmentation
		_json_doc.clear();

		_json_doc["hash"] = message.hash().toHex();
		_json_doc["destination_hash"] = message.destination_hash().toHex();
		_json_doc["source_hash"] = message.source_hash().toHex();
		_json_doc["incoming"] = message.incoming();
		_json_doc["timestamp"] = message.timestamp();
		_json_doc["state"] = static_cast<int>(message.state());

		// Store content as UTF-8 for fast loading (no msgpack unpacking needed)
		std::string content_str((const char*)message.content().data(), message.content().size());
		_json_doc["content"] = content_str;

		// Store the entire packed message to preserve hash/signature
		// This ensures exact reconstruction on load
		_json_doc["packed"] = message.packed().toHex();

		// Write message file via OS abstraction (SPIFFS compatible)
		std::string message_path = get_message_path(message.hash());
		std::string json_str;
		serializeJsonPretty(_json_doc, json_str);
		Bytes data((const uint8_t*)json_str.data(), json_str.size());

		if (Utilities::OS::write_file(message_path.c_str(), data) != data.size()) {
			ERROR("Failed to write message file: " + message_path);
			return false;
		}

		DEBUG("  Message file saved: " + message_path);

		// Update conversation index
		// Determine peer hash (the other party in the conversation)
		// For incoming: peer = source, for outgoing: peer = destination
		Bytes peer_hash = message.incoming() ? message.source_hash() : message.destination_hash();

		// Get or create conversation slot
		ConversationSlot* slot = get_or_create_conversation(peer_hash);
		if (!slot) {
			ERROR("Conversation pool is full, cannot add message");
			return false;
		}

		ConversationInfo& conv = slot->info;

		// Add message to conversation (if not already present)
		bool already_exists = conv.has_message(message.hash());

		if (!already_exists) {
			// Hard-cap: if the in-memory hash list is at MAX, evict
			// the oldest entry (frees one slot AND its archived file
			// on the SD card) so new messages aren't silently dropped.
			if (conv.message_count >= MAX_MESSAGES_PER_CONVERSATION) {
				evict_oldest_message(conv);
			}
			if (!conv.add_message_hash(message.hash())) {
				WARNING("Message pool full for conversation: " + peer_hash.toHex());
			} else {
				conv.last_activity = message.timestamp();
				conv.set_last_message_hash(message.hash());

				// Increment unread count for incoming messages
				if (message.incoming()) {
					conv.unread_count++;
				}

				DEBUG("  Added to conversation (now " + std::to_string(conv.message_count) + " messages)");
			}
		}

		// Save updated index
		save_index();

		// Cull this conversation down to HOT_MESSAGES_PER_CONVERSATION:
		// older messages are MOVED to the archive filesystem if one
		// has been set, otherwise just deleted from the hot filesystem.
		// In either case the hash stays in the in-memory index, so the
		// UI can still list older messages and load_message will fall
		// through to the archive on a miss.
		cull_conversation_to_hot(peer_hash);

		INFO("Message saved successfully");
		return true;

	} catch (const std::exception& e) {
		ERROR("Exception saving message: " + std::string(e.what()));
		return false;
	}
}

// ============================================================================
// Two-tier (hot + archive) storage helpers
// ============================================================================

void MessageStore::set_archive_filesystem(microStore::FileSystem fs,
                                          const std::string& base_path) {
	_archive_fs = fs;
	if (!base_path.empty()) {
		_archive_path = base_path;
	} else {
		// Default to the same base path used for the hot store. On
		// pyxis this resolves to "/lxmf" → archived files live at
		// "/lxmf/m/<hash>.j" on the SD card.
		_archive_path = _base_path;
	}
	if (_archive_fs) {
		// Pre-create the archive's directory layout so the first
		// archive_one_message call doesn't trip on a missing dir.
		// Mirror the hot side: the path used by get_message_path is
		// `/m/<12chars>.j`, so we need `<archive_path>/m/`.
		if (!_archive_path.empty() && !_archive_fs.exists(_archive_path.c_str())) {
			_archive_fs.mkdir(_archive_path.c_str());
		}
		std::string messages_dir = _archive_path + "/m";
		if (!_archive_fs.exists(messages_dir.c_str())) {
			_archive_fs.mkdir(messages_dir.c_str());
		}
		INFO("Archive filesystem set; archive_path=" + _archive_path);
	} else {
		INFO("Archive filesystem cleared");
	}
}

bool MessageStore::has_archive() const {
	return (bool)_archive_fs;
}

bool MessageStore::set_display_name(const Bytes& peer_hash,
                                    const std::string& display_name) {
	ConversationSlot* slot = find_conversation(peer_hash);
	if (!slot) return false;
	if (display_name.empty()) return false;
	if (display_name == slot->info.display_name) return false;  // No change

	strncpy(slot->info.display_name, display_name.c_str(), MAX_DISPLAY_NAME_LEN);
	slot->info.display_name[MAX_DISPLAY_NAME_LEN] = '\0';
	save_index();
	return true;
}

std::string MessageStore::get_display_name(const Bytes& peer_hash) const {
	const ConversationSlot* slot = find_conversation(peer_hash);
	if (!slot) return std::string();
	return std::string(slot->info.display_name);
}

std::string MessageStore::get_archive_message_path(const Bytes& message_hash) const {
	// Mirror the hot-side relative layout (`/m/<12chars>.j`) under the
	// archive prefix so we can copy bytes 1:1 between the two
	// filesystems. The hot path uses an absolute leading "/" because
	// LittleFS is mounted at root; on SD we want this nested under a
	// per-app dir, so the archive_path is prepended verbatim.
	return _archive_path + get_message_path(message_hash);
}

size_t MessageStore::read_archive_file(const char* path, Bytes& out) {
	if (!_archive_fs || !_archive_fs.exists(path)) return 0;
	microStore::File f = _archive_fs.open(path, microStore::File::ModeRead);
	if (!f) return 0;
	const size_t sz = f.size();
	if (sz == 0) { f.close(); return 0; }
	uint8_t* buf = out.writable(sz);
	size_t n_read = f.read(buf, sz);
	f.close();
	out.resize(n_read);
	return n_read;
}

size_t MessageStore::write_archive_file(const char* path, const Bytes& data) {
	if (!_archive_fs) return 0;
	microStore::File f = _archive_fs.open(path, microStore::File::ModeWrite);
	if (!f) return 0;
	size_t n = f.write(data.data(), data.size());
	f.close();
	return n;
}

bool MessageStore::archive_one_message(const Bytes& message_hash) {
	std::string hot_path = get_message_path(message_hash);
	if (!Utilities::OS::file_exists(hot_path.c_str())) {
		// Already gone (eg previously archived in a prior session).
		// Treat as success — the only side effect we care about is
		// "this hash no longer occupies hot flash."
		return true;
	}

	// If we have an archive, copy hot→archive before deleting the hot
	// copy. If the archive write fails, leave the hot copy alone so
	// the message isn't lost. If we have no archive, just delete the
	// hot copy (bounded in-flash storage, no historical scrollback).
	if (_archive_fs) {
		Bytes data;
		if (Utilities::OS::read_file(hot_path.c_str(), data) == 0) {
			WARNING("archive_one_message: read_file(hot) returned 0 for "
			        + hot_path);
			return false;
		}
		std::string arch_path = get_archive_message_path(message_hash);
		size_t written = write_archive_file(arch_path.c_str(), data);
		if (written != data.size()) {
			WARNING("archive_one_message: archive write short ("
			        + std::to_string(written) + "/"
			        + std::to_string(data.size()) + " bytes) for "
			        + arch_path + " — keeping hot copy");
			return false;
		}
		DEBUG("archive_one_message: " + message_hash.toHex().substr(0, 16)
		      + "... → archive (" + std::to_string(written) + " bytes)");
	}

	// Delete from hot. If this fails the message exists in BOTH places,
	// which is wasteful but not corrupting; the next compaction sweeps
	// it. We log for visibility.
	if (!Utilities::OS::remove_file(hot_path.c_str())) {
		WARNING("archive_one_message: remove_file(hot) failed for "
		        + hot_path);
	}
	return true;
}

bool MessageStore::evict_oldest_message(ConversationInfo& conv) {
	if (conv.message_count == 0) return false;

	// Index 0 is the oldest message in the conversation. With cull
	// running on every save, index 0 is normally already in archive
	// (or evicted entirely if no archive_fs is set). Either way, we
	// want it gone from BOTH tiers and from the in-memory list.
	Bytes oldest = conv.message_hash_bytes(0);
	if (oldest.size() == 0) return false;

	// Hot — likely already gone, but check & remove for safety.
	std::string hot_path = get_message_path(oldest);
	if (Utilities::OS::file_exists(hot_path.c_str())) {
		Utilities::OS::remove_file(hot_path.c_str());
	}
	// Archive — the actual storage location for old messages.
	if (_archive_fs) {
		std::string arch_path = get_archive_message_path(oldest);
		if (_archive_fs.exists(arch_path.c_str())) {
			_archive_fs.remove(arch_path.c_str());
		}
	}
	// Drop from the in-memory hash list (shifts all indices down).
	bool removed = conv.remove_message_hash(oldest);
	if (removed) {
		INFO("evict_oldest_message: hard-cap evicted "
		     + oldest.toHex().substr(0, 16) + "...");
	}
	return removed;
}

void MessageStore::cull_conversation_to_hot(const Bytes& peer_hash) {
	ConversationSlot* slot = find_conversation(peer_hash);
	if (!slot) return;
	ConversationInfo& conv = slot->info;
	if (conv.message_count <= HOT_MESSAGES_PER_CONVERSATION) return;

	// Messages are appended in chronological order, so the OLDEST live
	// at the lowest indices. Archive everything from index 0 up to
	// (count - HOT_MESSAGES_PER_CONVERSATION). The hashes stay in the
	// in-memory list; only the file location changes.
	size_t archive_count = conv.message_count - HOT_MESSAGES_PER_CONVERSATION;
	size_t archived = 0;
	for (size_t i = 0; i < archive_count; ++i) {
		Bytes hash = conv.message_hash_bytes(i);
		if (hash.size() == 0) continue;
		if (archive_one_message(hash)) ++archived;
	}
	if (archived > 0) {
		std::string verb = _archive_fs ? "archived" : "evicted";
		INFO("cull_conversation_to_hot: " + verb + " "
		     + std::to_string(archived) + " message(s) for peer "
		     + peer_hash.toHex().substr(0, 16) + "...");
	}
}

// Load message from storage
LXMessage MessageStore::load_message(const Bytes& message_hash) {
	if (!_initialized) {
		ERROR("MessageStore not initialized");
		return LXMessage(Bytes(), Bytes(), Bytes(), Bytes());
	}

	std::string message_path = get_message_path(message_hash);

	// Two-phase read: hot first, then archive. The archive path is
	// only consulted on a hot miss, so the common case (recent
	// scrollback within HOT_MESSAGES_PER_CONVERSATION) stays a
	// single internal-flash read.
	bool in_hot = Utilities::OS::file_exists(message_path.c_str());
	bool in_archive = !in_hot && _archive_fs
	                  && _archive_fs.exists(get_archive_message_path(message_hash).c_str());

	if (!in_hot && !in_archive) {
		WARNING("Message file not found: " + message_path
		        + (_archive_fs ? " (also missing from archive)" : ""));
		return LXMessage(Bytes(), Bytes(), Bytes(), Bytes());
	}

	try {
		Bytes data;
		size_t n_read = 0;
		if (in_hot) {
			n_read = Utilities::OS::read_file(message_path.c_str(), data);
		} else {
			std::string arch_path = get_archive_message_path(message_hash);
			n_read = read_archive_file(arch_path.c_str(), data);
			DEBUG("Loaded message from archive: " + message_hash.toHex().substr(0, 16) + "...");
		}
		if (n_read == 0) {
			ERROR("Failed to read message file: " + message_path);
			return LXMessage(Bytes(), Bytes(), Bytes(), Bytes());
		}

		// Use reusable document to reduce heap fragmentation
		_json_doc.clear();
		DeserializationError error = deserializeJson(_json_doc, data.data(), data.size());

		if (error) {
			ERROR("Failed to parse message file: " + std::string(error.c_str()));
			return LXMessage(Bytes(), Bytes(), Bytes(), Bytes());
		}

		// Unpack the message from stored packed bytes
		// This preserves the exact hash and signature
		Bytes packed;
		packed.assignHex(_json_doc["packed"].as<const char*>());

		// Skip signature validation - messages from storage were already validated when received
		LXMessage message = LXMessage::unpack_from_bytes(packed, LXMF::Type::Message::DIRECT, true);

		// Restore incoming flag from storage (unpack_from_bytes defaults to true)
		if (!_json_doc["incoming"].isNull()) {
			message.incoming(_json_doc["incoming"].as<bool>());
		}

		DEBUG("Loaded message: " + message_hash.toHex());
		return message;

	} catch (const std::exception& e) {
		ERROR("Exception loading message: " + std::string(e.what()));
		return LXMessage(Bytes(), Bytes(), Bytes(), Bytes());
	}
}

// Load only message metadata (fast path - no msgpack unpacking)
MessageStore::MessageMetadata MessageStore::load_message_metadata(const Bytes& message_hash) {
	MessageMetadata meta;
	meta.valid = false;

	if (!_initialized) {
		return meta;
	}

	std::string message_path = get_message_path(message_hash);

	// Two-phase read: hot first, then archive (same pattern as load_message).
	bool in_hot = Utilities::OS::file_exists(message_path.c_str());
	bool in_archive = !in_hot && _archive_fs
	                  && _archive_fs.exists(get_archive_message_path(message_hash).c_str());
	if (!in_hot && !in_archive) {
		return meta;
	}

	try {
		Bytes data;
		size_t n_read = 0;
		if (in_hot) {
			n_read = Utilities::OS::read_file(message_path.c_str(), data);
		} else {
			std::string arch_path = get_archive_message_path(message_hash);
			n_read = read_archive_file(arch_path.c_str(), data);
		}
		if (n_read == 0) {
			return meta;
		}

		// Use reusable document to reduce heap fragmentation
		_json_doc.clear();
		DeserializationError error = deserializeJson(_json_doc, data.data(), data.size());

		if (error) {
			return meta;
		}

		meta.hash = message_hash;

		// Read pre-extracted fields (no msgpack unpacking needed)
		if (_json_doc["content"].is<const char*>()) {
			meta.content = _json_doc["content"].as<std::string>();
		}
		meta.timestamp = _json_doc["timestamp"] | 0.0;
		meta.incoming = _json_doc["incoming"] | true;
		meta.state = _json_doc["state"] | 0;
		meta.valid = true;

		return meta;

	} catch (...) {
		return meta;
	}
}

// Update message state in storage
bool MessageStore::update_message_state(const Bytes& message_hash, Type::Message::State state) {
	if (!_initialized) {
		ERROR("MessageStore not initialized");
		return false;
	}

	std::string message_path = get_message_path(message_hash);

	if (!Utilities::OS::file_exists(message_path.c_str())) {
		// State updates only matter for messages still in flight (DELIVERED,
		// FAILED, etc). Once a message is archived it's terminal — silently
		// skip the update rather than thrashing the SD card with a 1-byte
		// rewrite.
		if (_archive_fs && _archive_fs.exists(get_archive_message_path(message_hash).c_str())) {
			DEBUG("update_message_state: skipping archived message "
			      + message_hash.toHex().substr(0, 16) + "...");
			return true;
		}
		WARNING("Message file not found: " + message_path);
		return false;
	}

	try {
		// Read existing JSON
		Bytes data;
		if (Utilities::OS::read_file(message_path.c_str(), data) == 0) {
			ERROR("Failed to read message file: " + message_path);
			return false;
		}

		// Use reusable document to reduce heap fragmentation
		_json_doc.clear();
		DeserializationError error = deserializeJson(_json_doc, data.data(), data.size());
		if (error) {
			ERROR("Failed to parse message file: " + std::string(error.c_str()));
			return false;
		}

		// Update state
		_json_doc["state"] = static_cast<int>(state);

		// Write back
		std::string json_str;
		serializeJson(_json_doc, json_str);
		if (!Utilities::OS::write_file(message_path.c_str(), Bytes((uint8_t*)json_str.c_str(), json_str.length()))) {
			ERROR("Failed to write message file: " + message_path);
			return false;
		}

		INFO("Message state updated to " + std::to_string(static_cast<int>(state)));
		return true;

	} catch (const std::exception& e) {
		ERROR("Exception updating message state: " + std::string(e.what()));
		return false;
	}
}

// Delete message from storage
bool MessageStore::delete_message(const Bytes& message_hash) {
	if (!_initialized) {
		ERROR("MessageStore not initialized");
		return false;
	}

	INFO("Deleting message: " + message_hash.toHex());

	// Remove message file from BOTH hot and archive — the file might
	// be in either tier (or even both, briefly, if a previous archive
	// op got past the write but failed the hot delete).
	std::string message_path = get_message_path(message_hash);
	if (Utilities::OS::file_exists(message_path.c_str())) {
		if (!Utilities::OS::remove_file(message_path.c_str())) {
			ERROR("Failed to delete message file: " + message_path);
			return false;
		}
	}
	if (_archive_fs) {
		std::string arch_path = get_archive_message_path(message_hash);
		if (_archive_fs.exists(arch_path.c_str())) {
			if (!_archive_fs.remove(arch_path.c_str())) {
				WARNING("Failed to delete archived message file: " + arch_path);
				// Hot copy already gone; logical state is "deleted"
				// even if the archive blob lingers. Don't fail the
				// caller — better to leave a dangling file than to
				// abort a delete that the user explicitly asked for.
			}
		}
	}

	// Update conversation index - remove from all conversations
	for (size_t i = 0; i < MAX_CONVERSATIONS; ++i) {
		ConversationSlot& slot = _conversations_pool[i];
		if (!slot.in_use) {
			continue;
		}

		ConversationInfo& conv = slot.info;
		if (conv.remove_message_hash(message_hash)) {
			// Update last message if this was it
			if (conv.last_message_hash_bytes() == message_hash) {
				if (conv.message_count > 0) {
					conv.set_last_message_hash(conv.message_hash_bytes(conv.message_count - 1));
				} else {
					memset(conv.last_message_hash, 0, MESSAGE_HASH_SIZE);
				}
			}

			DEBUG("  Removed from conversation");
			break;
		}
	}

	save_index();
	INFO("Message deleted");
	return true;
}

// Get list of conversations (sorted by last activity)
std::vector<Bytes> MessageStore::get_conversations() {
	std::vector<std::pair<double, Bytes>> sorted;

	for (size_t i = 0; i < MAX_CONVERSATIONS; ++i) {
		const ConversationSlot& slot = _conversations_pool[i];
		if (slot.in_use) {
			sorted.push_back({slot.info.last_activity, slot.peer_hash_bytes()});
		}
	}

	// Sort by last activity (most recent first)
	std::sort(sorted.begin(), sorted.end(),
		[](const std::pair<double, Bytes>& a, const std::pair<double, Bytes>& b) { return a.first > b.first; });

	std::vector<Bytes> result;
	for (const auto& pair : sorted) {
		result.push_back(pair.second);
	}

	return result;
}

// Get conversation info
MessageStore::ConversationInfo MessageStore::get_conversation_info(const Bytes& peer_hash) {
	const ConversationSlot* slot = find_conversation(peer_hash);
	if (slot) {
		return slot->info;
	}
	return ConversationInfo();
}

// Get messages for conversation
std::vector<Bytes> MessageStore::get_messages_for_conversation(const Bytes& peer_hash) {
	const ConversationSlot* slot = find_conversation(peer_hash);
	if (slot) {
		std::vector<Bytes> result;
		result.reserve(slot->info.message_count);
		for (size_t i = 0; i < slot->info.message_count; ++i) {
			result.push_back(slot->info.message_hash_bytes(i));
		}
		return result;
	}
	return std::vector<Bytes>();
}

// Mark conversation as read
void MessageStore::mark_conversation_read(const Bytes& peer_hash) {
	ConversationSlot* slot = find_conversation(peer_hash);
	if (slot) {
		slot->info.unread_count = 0;
		save_index();
		DEBUG("Marked conversation as read: " + peer_hash.toHex());
	}
}

// Delete entire conversation
bool MessageStore::delete_conversation(const Bytes& peer_hash) {
	ConversationSlot* slot = find_conversation(peer_hash);
	if (!slot) {
		WARNING("Conversation not found: " + peer_hash.toHex());
		return false;
	}

	INFO("Deleting conversation: " + peer_hash.toHex());

	// Delete all message files from BOTH hot and archive.
	for (size_t i = 0; i < slot->info.message_count; ++i) {
		Bytes msg_hash = slot->info.message_hash_bytes(i);
		std::string message_path = get_message_path(msg_hash);
		if (Utilities::OS::file_exists(message_path.c_str())) {
			Utilities::OS::remove_file(message_path.c_str());
		}
		if (_archive_fs) {
			std::string arch_path = get_archive_message_path(msg_hash);
			if (_archive_fs.exists(arch_path.c_str())) {
				_archive_fs.remove(arch_path.c_str());
			}
		}
	}

	// Clear slot and mark as not in use
	slot->clear();
	save_index();

	INFO("Conversation deleted");
	return true;
}

// Get total message count
size_t MessageStore::get_message_count() const {
	size_t count = 0;
	for (size_t i = 0; i < MAX_CONVERSATIONS; ++i) {
		if (_conversations_pool[i].in_use) {
			count += _conversations_pool[i].info.message_count;
		}
	}
	return count;
}

// Get conversation count
size_t MessageStore::get_conversation_count() const {
	return count_conversations();
}

// Get total unread count
size_t MessageStore::get_unread_count() const {
	size_t count = 0;
	for (size_t i = 0; i < MAX_CONVERSATIONS; ++i) {
		if (_conversations_pool[i].in_use) {
			count += _conversations_pool[i].info.unread_count;
		}
	}
	return count;
}

// Clear all data
bool MessageStore::clear_all() {
	INFO("Clearing all message store data");

	// Delete all message files
	for (size_t i = 0; i < MAX_CONVERSATIONS; ++i) {
		ConversationSlot& slot = _conversations_pool[i];
		if (!slot.in_use) {
			continue;
		}
		for (size_t j = 0; j < slot.info.message_count; ++j) {
			std::string message_path = get_message_path(slot.info.message_hash_bytes(j));
			if (Utilities::OS::file_exists(message_path.c_str())) {
				Utilities::OS::remove_file(message_path.c_str());
			}
		}
		slot.clear();
	}

	// Save empty index
	save_index();

	INFO("Message store cleared");
	return true;
}

// Get message file path
// Use short path for SPIFFS compatibility (32 char filename limit)
// Format: /m/<first12chars>.j (12 chars of hash = 6 bytes = plenty unique for local store)
std::string MessageStore::get_message_path(const Bytes& message_hash) const {
	return "/m/" + message_hash.toHex().substr(0, 12) + ".j";
}

// Get conversation directory path
std::string MessageStore::get_conversation_path(const Bytes& peer_hash) const {
	return "/c/" + peer_hash.toHex().substr(0, 12);
}

// Determine peer hash from message
Bytes MessageStore::get_peer_hash(const LXMessage& message, const Bytes& our_hash) const {
	// For incoming messages: peer = source
	// For outgoing messages: peer = destination
	if (message.incoming()) {
		return message.source_hash();
	} else {
		return message.destination_hash();
	}
}

// Find a conversation slot by peer hash
MessageStore::ConversationSlot* MessageStore::find_conversation(const Bytes& peer_hash) {
	for (size_t i = 0; i < MAX_CONVERSATIONS; ++i) {
		if (_conversations_pool[i].in_use && _conversations_pool[i].peer_hash_equals(peer_hash)) {
			return &_conversations_pool[i];
		}
	}
	return nullptr;
}

const MessageStore::ConversationSlot* MessageStore::find_conversation(const Bytes& peer_hash) const {
	for (size_t i = 0; i < MAX_CONVERSATIONS; ++i) {
		if (_conversations_pool[i].in_use && _conversations_pool[i].peer_hash_equals(peer_hash)) {
			return &_conversations_pool[i];
		}
	}
	return nullptr;
}

// Get or create a conversation slot for a peer
MessageStore::ConversationSlot* MessageStore::get_or_create_conversation(const Bytes& peer_hash) {
	// First try to find existing
	ConversationSlot* slot = find_conversation(peer_hash);
	if (slot) {
		return slot;
	}

	// Find a free slot
	for (size_t i = 0; i < MAX_CONVERSATIONS; ++i) {
		if (!_conversations_pool[i].in_use) {
			_conversations_pool[i].in_use = true;
			_conversations_pool[i].set_peer_hash(peer_hash);
			_conversations_pool[i].info.set_peer_hash(peer_hash);
			DEBUG("  Created new conversation with: " + peer_hash.toHex());
			return &_conversations_pool[i];
		}
	}

	return nullptr;  // Pool is full
}

// Count number of active conversations in pool
size_t MessageStore::count_conversations() const {
	size_t count = 0;
	for (size_t i = 0; i < MAX_CONVERSATIONS; ++i) {
		if (_conversations_pool[i].in_use) {
			++count;
		}
	}
	return count;
}
