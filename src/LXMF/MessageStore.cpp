#include "MessageStore.h"
#include <microReticulum/Log.h>
#include <microReticulum/Utilities/OS.h>

#include <ArduinoJson.h>
#include <algorithm>
#include <set>
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

	if (initialize_storage() && load_index()) {
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

bool MessageStore::load_index() {
	std::string index_path = "/conv.json";  // Short path for SPIFFS
	std::string temp_path = "/conv.tmp";
	std::string backup_path = "/conv.bak";
	if (Utilities::OS::file_exists(temp_path.c_str())) {
		Utilities::OS::remove_file(temp_path.c_str());
	}

	bool has_index = Utilities::OS::file_exists(index_path.c_str());
	bool has_backup = Utilities::OS::file_exists(backup_path.c_str());
	if (!has_index && !has_backup) {
		DEBUG("No existing conversation index found");
		return true;
	}

	if (has_index && load_index_file(index_path)) {
		if (has_backup) Utilities::OS::remove_file(backup_path.c_str());
		return true;
	}

	if (has_backup && load_index_file(backup_path)) {
		if (has_index && !Utilities::OS::remove_file(index_path.c_str())) {
			ERROR("Failed to remove invalid conversation index during recovery");
			return false;
		}
		if (!Utilities::OS::rename_file(backup_path.c_str(), index_path.c_str())) {
			ERROR("Failed to restore validated conversation index backup");
			return false;
		}
		INFO("Recovered conversation index from validated backup");
		return true;
	}

	ERROR("No valid conversation index generation is available");
	return false;
}

// Parse one index generation. The caller decides whether live or backup wins.
bool MessageStore::load_index_file(const std::string& index_path) {
	auto reject_index = [this](const std::string& reason) {
		ERROR(reason);
		for (size_t i = 0; i < MAX_CONVERSATIONS; ++i) {
			_conversations_pool[i].clear();
		}
		return false;
	};

	try {
		// Read JSON file via OS abstraction (SPIFFS compatible)
		Bytes data;
		if (Utilities::OS::read_file(index_path.c_str(), data) == 0) {
			ERROR("Failed to read index file or index is empty: " + index_path);
			return false;
		}

		// Parse JSON from bytes using reusable document to reduce heap fragmentation
		_json_doc.clear();
		DeserializationError error = deserializeJson(_json_doc, data.data(), data.size());

		if (error) {
			ERROR("Failed to parse conversation index: " + std::string(error.c_str()));
			return false;
		}

		// Load conversations into pool
		if (!_json_doc["conversations"].is<JsonArray>()) {
			return reject_index("Conversation index has no conversations array");
		}
		JsonArray conversations = _json_doc["conversations"].as<JsonArray>();
		if (conversations.size() > MAX_CONVERSATIONS) {
			return reject_index("Conversation index exceeds fixed pool capacity");
		}
		std::set<std::string> peer_hashes;
		std::set<std::string> message_hashes;
		size_t slot_index = 0;
		for (JsonObject conv : conversations) {
			ConversationSlot& slot = _conversations_pool[slot_index];

			// Parse peer hash
			const char* peer_hex = conv["peer_hash"];
			if (!peer_hex) {
				return reject_index("Conversation index entry has no peer hash");
			}
			Bytes peer_bytes;
			peer_bytes.assignHex(peer_hex);
			if (peer_bytes.size() != PEER_HASH_SIZE) {
				return reject_index("Conversation index entry has invalid peer hash");
			}
			if (!peer_hashes.insert(peer_bytes.toHex()).second) {
				return reject_index("Conversation index contains a duplicate peer hash");
			}
			slot.in_use = true;
			slot.set_peer_hash(peer_bytes);
			slot.info.set_peer_hash(peer_bytes);

			// Parse message hashes
			if (!conv["messages"].is<JsonArray>()) {
				return reject_index("Conversation index entry has no messages array");
			}
			JsonArray messages = conv["messages"].as<JsonArray>();
			if (messages.size() > MAX_MESSAGES_PER_CONVERSATION) {
				return reject_index("Conversation index entry exceeds message capacity");
			}
			std::set<std::string> conversation_messages;
			for (const char* msg_hex : messages) {
				if (!msg_hex) return reject_index("Conversation index has a null message hash");
				Bytes msg_hash;
				msg_hash.assignHex(msg_hex);
				if (msg_hash.size() != MESSAGE_HASH_SIZE) {
					return reject_index("Conversation index has an invalid message hash");
				}
				std::string canonical_hash = msg_hash.toHex();
				if (!conversation_messages.insert(canonical_hash).second ||
				    !message_hashes.insert(canonical_hash).second ||
				    !slot.info.add_message_hash(msg_hash)) {
					return reject_index("Conversation index contains a duplicate message hash");
				}
			}

			// Parse metadata
			slot.info.last_activity = conv["last_activity"] | 0.0;
			slot.info.unread_count = conv["unread_count"] | 0;

			if (!conv["last_message_hash"].isNull()) {
				const char* last_msg_hex = conv["last_message_hash"];
				if (!last_msg_hex) return reject_index("Conversation index has a null last-message hash");
				Bytes last_msg_bytes;
				last_msg_bytes.assignHex(last_msg_hex);
				if (last_msg_bytes.size() != MESSAGE_HASH_SIZE) {
					return reject_index("Conversation index has an invalid last-message hash");
				}
				if (conversation_messages.count(last_msg_bytes.toHex()) == 0) {
					return reject_index("Last-message hash is not present in its conversation");
				}
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
		return true;

	} catch (const std::exception& e) {
		return reject_index("Exception loading conversation index: " + std::string(e.what()));
	}
}

// Save conversation index to disk
bool MessageStore::save_index(bool empty) {
	std::string index_path = "/conv.json";  // Short path for SPIFFS
	std::string temp_path = "/conv.tmp";
	std::string backup_path = "/conv.bak";

	try {
		// Use reusable document to reduce heap fragmentation
		_json_doc.clear();
		JsonArray conversations = _json_doc["conversations"].to<JsonArray>();

		// Serialize each active conversation from pool
		for (size_t i = 0; i < MAX_CONVERSATIONS; ++i) {
			if (empty) break;
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

		// Never truncate the only committed index in place. Write and verify a
		// temporary file, then replace the index with a recoverable two-rename
		// transaction. load_index() restores /conv.bak after an interrupted boot.
		if (Utilities::OS::write_file(temp_path.c_str(), data) != data.size()) {
			ERROR("Failed to write temporary index file: " + temp_path);
			Utilities::OS::remove_file(temp_path.c_str());
			return false;
		}

		Bytes verify;
		if (Utilities::OS::read_file(temp_path.c_str(), verify) != data.size() ||
		    verify.size() != data.size() ||
		    memcmp(verify.data(), data.data(), data.size()) != 0) {
			ERROR("Failed to verify temporary conversation index");
			Utilities::OS::remove_file(temp_path.c_str());
			return false;
		}
		_json_doc.clear();
		if (deserializeJson(_json_doc, verify.data(), verify.size()) ||
		    !_json_doc["conversations"].is<JsonArray>()) {
			ERROR("Temporary conversation index failed parse validation");
			Utilities::OS::remove_file(temp_path.c_str());
			return false;
		}

		if (Utilities::OS::file_exists(backup_path.c_str())) {
			Utilities::OS::remove_file(backup_path.c_str());
		}
		bool had_index = Utilities::OS::file_exists(index_path.c_str());
		if (had_index &&
		    !Utilities::OS::rename_file(index_path.c_str(), backup_path.c_str())) {
			ERROR("Failed to preserve previous conversation index");
			Utilities::OS::remove_file(temp_path.c_str());
			return false;
		}
		if (!Utilities::OS::rename_file(temp_path.c_str(), index_path.c_str())) {
			ERROR("Failed to commit conversation index");
			if (had_index) {
				Utilities::OS::rename_file(backup_path.c_str(), index_path.c_str());
			}
			Utilities::OS::remove_file(temp_path.c_str());
			return false;
		}
		if (had_index) {
			Utilities::OS::remove_file(backup_path.c_str());
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
	std::string message_path = get_message_path(message.hash());
	size_t extension_pos = message_path.rfind('.');
	std::string message_stem = message_path.substr(0, extension_pos);
	std::string temp_message_path = message_stem + ".tmp";
	std::string backup_message_path = message_stem + ".bak";
	bool had_payload = false;
	bool payload_committed = false;
	bool index_committed = false;
	ConversationSlot* transaction_slot = nullptr;
	bool conversation_snapshot = false;
	bool created_conversation = false;
	auto rollback_payload = [&]() {
		if (!payload_committed) return;
		Utilities::OS::remove_file(message_path.c_str());
		if (had_payload) {
			if (!Utilities::OS::rename_file(backup_message_path.c_str(), message_path.c_str())) {
				ERROR("Failed to restore previous message payload after rollback");
			}
		} else if (Utilities::OS::file_exists(backup_message_path.c_str())) {
			Utilities::OS::remove_file(backup_message_path.c_str());
		}
		payload_committed = false;
	};

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

		std::string json_str;
		serializeJsonPretty(_json_doc, json_str);
		Bytes data((const uint8_t*)json_str.data(), json_str.size());

		// Validate both generations before discarding either one. This also
		// rejects a different full hash sharing the shortened filename.
		bool has_payload_generation =
			Utilities::OS::file_exists(message_path.c_str()) ||
			Utilities::OS::file_exists(backup_message_path.c_str());
		if (has_payload_generation &&
		    !recover_message_payload(message_path, message.hash())) {
			return false;
		}
		had_payload = Utilities::OS::file_exists(message_path.c_str());

		if (_archive_fs) {
			std::string archive_path = get_archive_message_path(message.hash());
			size_t archive_extension = archive_path.rfind('.');
			std::string archive_backup = archive_path.substr(0, archive_extension) + ".bak";
			bool has_archive_generation = _archive_fs.exists(archive_path.c_str()) ||
			                              _archive_fs.exists(archive_backup.c_str());
			if (has_archive_generation) {
				if (!recover_archived_message_payload(message.hash())) return false;
				Bytes archived;
				if (read_archive_file(archive_path.c_str(), archived) == 0) {
					ERROR("Archived message payload is unreadable; refusing overwrite");
					return false;
				}
				_json_doc.clear();
				if (deserializeJson(_json_doc, archived.data(), archived.size())) {
					ERROR("Archived message payload is malformed; refusing overwrite");
					return false;
				}
				const char* archived_hash = _json_doc["hash"];
				if (!archived_hash || message.hash().toHex() != archived_hash) {
					ERROR("Archived message filename collision detected");
					return false;
				}
			}
		}

		if (Utilities::OS::write_file(temp_message_path.c_str(), data) != data.size()) {
			ERROR("Failed to write temporary message file: " + temp_message_path);
			Utilities::OS::remove_file(temp_message_path.c_str());
			return false;
		}
		Bytes verify_payload;
		if (Utilities::OS::read_file(temp_message_path.c_str(), verify_payload) != data.size() ||
		    verify_payload.size() != data.size() ||
		    memcmp(verify_payload.data(), data.data(), data.size()) != 0) {
			ERROR("Failed to verify temporary message payload");
			Utilities::OS::remove_file(temp_message_path.c_str());
			return false;
		}
		_json_doc.clear();
		if (deserializeJson(_json_doc, verify_payload.data(), verify_payload.size())) {
			ERROR("Temporary message payload is malformed");
			Utilities::OS::remove_file(temp_message_path.c_str());
			return false;
		}
		const char* verified_hash = _json_doc["hash"];
		if (!verified_hash || message.hash().toHex() != verified_hash) {
			ERROR("Temporary message payload hash validation failed");
			Utilities::OS::remove_file(temp_message_path.c_str());
			return false;
		}

		if (had_payload &&
		    !Utilities::OS::rename_file(message_path.c_str(), backup_message_path.c_str())) {
			ERROR("Failed to preserve previous message payload");
			Utilities::OS::remove_file(temp_message_path.c_str());
			return false;
		}
		if (!Utilities::OS::rename_file(temp_message_path.c_str(), message_path.c_str())) {
			ERROR("Failed to commit message payload");
			if (had_payload) {
				Utilities::OS::rename_file(backup_message_path.c_str(), message_path.c_str());
			}
			Utilities::OS::remove_file(temp_message_path.c_str());
			return false;
		}
		payload_committed = true;

		DEBUG("  Message file saved: " + message_path);

		// Update conversation index
		// Determine peer hash (the other party in the conversation)
		// For incoming: peer = source, for outgoing: peer = destination
		Bytes peer_hash = message.incoming() ? message.source_hash() : message.destination_hash();

		// Get or create conversation slot
		transaction_slot = find_conversation(peer_hash);
		created_conversation = (transaction_slot == nullptr);
		if (!transaction_slot) {
			transaction_slot = get_or_create_conversation(peer_hash);
		}
		if (!transaction_slot) {
			ERROR("Conversation pool is full, cannot add message");
			rollback_payload();
			return false;
		}

		ConversationInfo& conv = transaction_slot->info;
		_transaction_snapshot = conv;
		conversation_snapshot = true;

		// Add message to conversation (if not already present)
		bool already_exists = conv.has_message(message.hash());
		Bytes evicted_hash;

		if (!already_exists) {
			// Hard-cap: remove the oldest hash from the candidate index, but
			// defer deleting its payload until the new index commits. This
			// keeps rollback possible when storage fails.
			if (conv.message_count >= MAX_MESSAGES_PER_CONVERSATION) {
				evicted_hash = conv.message_hash_bytes(0);
				if (!evicted_hash || !conv.remove_message_hash(evicted_hash)) {
					ERROR("Unable to reserve conversation slot for new message");
					rollback_payload();
					conv = _transaction_snapshot;
					return false;
				}
			}
			if (!conv.add_message_hash(message.hash())) {
				ERROR("Message pool full for conversation: " + peer_hash.toHex());
				rollback_payload();
				conv = _transaction_snapshot;
				return false;
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

		// A message is durable only when both its payload and the conversation
		// index are committed. Roll back the in-memory mutation and orphaned
		// payload when the index transaction fails.
		if (!save_index()) {
			ERROR("Message payload written but conversation index commit failed");
			rollback_payload();
			if (created_conversation) {
				transaction_slot->clear();
			} else {
				conv = _transaction_snapshot;
			}
			return false;
		}
		index_committed = true;
		if (had_payload && Utilities::OS::file_exists(backup_message_path.c_str())) {
			Utilities::OS::remove_file(backup_message_path.c_str());
		}
		payload_committed = false;

		// The replacement index is now durable. It is safe to remove the
		// hard-cap payload that the committed index no longer references.
		if (evicted_hash) {
			std::string hot_path = get_message_path(evicted_hash);
			if (Utilities::OS::file_exists(hot_path.c_str())) {
				Utilities::OS::remove_file(hot_path.c_str());
			}
			if (_archive_fs) {
				std::string archive_path = get_archive_message_path(evicted_hash);
				if (_archive_fs.exists(archive_path.c_str())) {
					_archive_fs.remove(archive_path.c_str());
				}
			}
			INFO("Hard-cap evicted committed message "
			     + evicted_hash.toHex().substr(0, 16) + "...");
		}

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
		if (index_committed) {
			ERROR("Post-commit message cleanup failed: " + std::string(e.what()));
			return true;
		}
		rollback_payload();
		if (conversation_snapshot && transaction_slot) {
			if (created_conversation) transaction_slot->clear();
			else transaction_slot->info = _transaction_snapshot;
		}
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
		return true;
	}

	if (_archive_fs) {
		Bytes data;
		if (Utilities::OS::read_file(hot_path.c_str(), data) == 0) {
			WARNING("archive_one_message: unable to read hot payload " + hot_path);
			return false;
		}
		_json_doc.clear();
		if (deserializeJson(_json_doc, data.data(), data.size())) return false;
		const char* hot_hash = _json_doc["hash"];
		if (!hot_hash || message_hash.toHex() != hot_hash) {
			ERROR("archive_one_message: hot filename collision detected");
			return false;
		}

		std::string arch_path = get_archive_message_path(message_hash);
		size_t extension_pos = arch_path.rfind('.');
		std::string stem = arch_path.substr(0, extension_pos);
		std::string temp_path = stem + ".tmp";
		std::string backup_path = stem + ".bak";
		bool has_archive_generation = _archive_fs.exists(arch_path.c_str()) ||
		                              _archive_fs.exists(backup_path.c_str());
		if (has_archive_generation &&
		    !recover_archived_message_payload(message_hash)) return false;
		bool had_archive = _archive_fs.exists(arch_path.c_str());
		if (had_archive) {
			Bytes existing;
			if (read_archive_file(arch_path.c_str(), existing) == 0) return false;
			_json_doc.clear();
			if (deserializeJson(_json_doc, existing.data(), existing.size())) return false;
			const char* archived_hash = _json_doc["hash"];
			if (!archived_hash || message_hash.toHex() != archived_hash) {
				ERROR("archive_one_message: archive filename collision detected");
				return false;
			}
		}

		_archive_fs.remove(temp_path.c_str());
		_archive_fs.remove(backup_path.c_str());
		if (write_archive_file(temp_path.c_str(), data) != data.size()) {
			_archive_fs.remove(temp_path.c_str());
			return false;
		}
		Bytes verify;
		if (read_archive_file(temp_path.c_str(), verify) != data.size()) {
			_archive_fs.remove(temp_path.c_str());
			return false;
		}
		_json_doc.clear();
		if (deserializeJson(_json_doc, verify.data(), verify.size())) {
			_archive_fs.remove(temp_path.c_str());
			return false;
		}
		const char* verified_hash = _json_doc["hash"];
		if (!verified_hash || message_hash.toHex() != verified_hash) {
			_archive_fs.remove(temp_path.c_str());
			return false;
		}
		if (had_archive && !_archive_fs.rename(arch_path.c_str(), backup_path.c_str())) {
			_archive_fs.remove(temp_path.c_str());
			return false;
		}
		if (!_archive_fs.rename(temp_path.c_str(), arch_path.c_str())) {
			if (had_archive) _archive_fs.rename(backup_path.c_str(), arch_path.c_str());
			_archive_fs.remove(temp_path.c_str());
			return false;
		}
		if (had_archive) _archive_fs.remove(backup_path.c_str());
		DEBUG("archive_one_message: " + message_hash.toHex().substr(0, 16)
		      + "... committed to archive");
	}

	if (!Utilities::OS::remove_file(hot_path.c_str())) {
		WARNING("archive_one_message: remove_file(hot) failed for " + hot_path);
	}
	return true;
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

bool MessageStore::recover_message_payload(const std::string& message_path,
                                           const Bytes& expected_hash) {
	size_t extension_pos = message_path.rfind('.');
	std::string stem = message_path.substr(0, extension_pos);
	std::string temp_path = stem + ".tmp";
	std::string backup_path = stem + ".bak";
	bool has_payload = Utilities::OS::file_exists(message_path.c_str());
	bool has_backup = Utilities::OS::file_exists(backup_path.c_str());
	auto valid_payload = [&](const std::string& path) {
		Bytes data;
		if (Utilities::OS::read_file(path.c_str(), data) == 0) return false;
		_json_doc.clear();
		if (deserializeJson(_json_doc, data.data(), data.size())) return false;
		const char* stored_hash = _json_doc["hash"];
		return stored_hash && expected_hash.toHex() == stored_hash;
	};

	if (has_payload && valid_payload(message_path)) {
		if (has_backup) Utilities::OS::remove_file(backup_path.c_str());
	} else if (has_backup && valid_payload(backup_path)) {
		if (has_payload && !Utilities::OS::remove_file(message_path.c_str())) {
			ERROR("Failed to remove invalid message payload during recovery");
			return false;
		}
		if (!Utilities::OS::rename_file(backup_path.c_str(), message_path.c_str())) {
			ERROR("Failed to restore validated message payload backup");
			return false;
		}
		has_payload = true;
	} else if (has_payload || has_backup) {
		ERROR("No valid message payload generation is available");
		return false;
	}
	if (Utilities::OS::file_exists(temp_path.c_str())) {
		Utilities::OS::remove_file(temp_path.c_str());
	}
	return has_payload;
}

bool MessageStore::recover_archived_message_payload(const Bytes& expected_hash) {
	if (!_archive_fs) return false;
	std::string path = get_archive_message_path(expected_hash);
	size_t extension_pos = path.rfind('.');
	std::string stem = path.substr(0, extension_pos);
	std::string temp = stem + ".tmp";
	std::string backup = stem + ".bak";
	bool has_payload = _archive_fs.exists(path.c_str());
	bool has_backup = _archive_fs.exists(backup.c_str());
	auto valid = [&](const std::string& candidate) {
		Bytes data;
		if (read_archive_file(candidate.c_str(), data) == 0) return false;
		_json_doc.clear();
		if (deserializeJson(_json_doc, data.data(), data.size())) return false;
		const char* stored_hash = _json_doc["hash"];
		return stored_hash && expected_hash.toHex() == stored_hash;
	};
	if (has_payload && valid(path)) {
		if (has_backup) _archive_fs.remove(backup.c_str());
	} else if (has_backup && valid(backup)) {
		if (has_payload && !_archive_fs.remove(path.c_str())) return false;
		if (!_archive_fs.rename(backup.c_str(), path.c_str())) return false;
		has_payload = true;
	} else if (has_payload || has_backup) {
		return false;
	}
	if (_archive_fs.exists(temp.c_str())) _archive_fs.remove(temp.c_str());
	return has_payload;
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
	bool in_hot = recover_message_payload(message_path, message_hash);
	bool in_archive = !in_hot && recover_archived_message_payload(message_hash);

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
		const char* stored_hash = _json_doc["hash"];
		if (!stored_hash || message_hash.toHex() != stored_hash) {
			ERROR("Loaded message payload does not match requested full hash");
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
		if (!_json_doc["state"].isNull()) {
			message.state(static_cast<Type::Message::State>(_json_doc["state"].as<int>()));
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

	try {
		// Read the hot file directly. read_file() returns 0 for a missing file
		// (filesystem.size()==0, no error), so a separate file_exists() probe is
		// unnecessary — that probe was a third LittleFS open per message (after
		// size() + readFile()), and opens dominate conversation-load time. Fall
		// back to the archive only when the hot read comes up empty.
		Bytes data;
		size_t extension_pos = message_path.rfind('.');
		std::string backup_path = message_path.substr(0, extension_pos) + ".bak";
		if (Utilities::OS::file_exists(backup_path.c_str())) {
			recover_message_payload(message_path, message_hash);
		}
		size_t n_read = Utilities::OS::read_file(message_path.c_str(), data);
		if (n_read == 0 && recover_message_payload(message_path, message_hash)) {
			n_read = Utilities::OS::read_file(message_path.c_str(), data);
		}
		if (n_read == 0 && recover_archived_message_payload(message_hash)) {
			std::string arch_path = get_archive_message_path(message_hash);
			n_read = read_archive_file(arch_path.c_str(), data);
		}
		if (n_read == 0) {
			return meta;
		}

		// Parse only the metadata fields, skipping the large "packed" hex blob
		// (the full message payload) — filtering stops deserializeJson from
		// walking the biggest value in the document.
		JsonDocument filter;
		filter["hash"] = true;
		filter["content"] = true;
		filter["timestamp"] = true;
		filter["incoming"] = true;
		filter["state"] = true;
		_json_doc.clear();
		DeserializationError error = deserializeJson(_json_doc, data.data(), data.size(),
		                                             DeserializationOption::Filter(filter));

		if (error) {
			return meta;
		}
		const char* stored_hash = _json_doc["hash"];
		if (!stored_hash || message_hash.toHex() != stored_hash) return meta;

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

bool MessageStore::update_archived_message_state(
    const Bytes& message_hash, Type::Message::State state) {
	std::string path = get_archive_message_path(message_hash);
	Bytes data;
	if (read_archive_file(path.c_str(), data) == 0) return false;
	_json_doc.clear();
	if (deserializeJson(_json_doc, data.data(), data.size())) return false;
	const char* stored_hash = _json_doc["hash"];
	if (!stored_hash || message_hash.toHex() != stored_hash) {
		ERROR("Archived state update rejected due to filename collision");
		return false;
	}
	_json_doc["state"] = static_cast<int>(state);
	std::string json;
	serializeJson(_json_doc, json);
	Bytes replacement((const uint8_t*)json.data(), json.size());
	size_t extension_pos = path.rfind('.');
	std::string stem = path.substr(0, extension_pos);
	std::string temp = stem + ".tmp";
	std::string backup = stem + ".bak";
	_archive_fs.remove(temp.c_str());
	_archive_fs.remove(backup.c_str());
	if (write_archive_file(temp.c_str(), replacement) != replacement.size()) {
		_archive_fs.remove(temp.c_str());
		return false;
	}
	Bytes verify;
	if (read_archive_file(temp.c_str(), verify) != replacement.size()) {
		_archive_fs.remove(temp.c_str());
		return false;
	}
	_json_doc.clear();
	if (deserializeJson(_json_doc, verify.data(), verify.size()) ||
	    (_json_doc["state"] | -1) != static_cast<int>(state)) {
		_archive_fs.remove(temp.c_str());
		return false;
	}
	if (!_archive_fs.rename(path.c_str(), backup.c_str())) {
		_archive_fs.remove(temp.c_str());
		return false;
	}
	if (!_archive_fs.rename(temp.c_str(), path.c_str())) {
		_archive_fs.rename(backup.c_str(), path.c_str());
		_archive_fs.remove(temp.c_str());
		return false;
	}
	_archive_fs.remove(backup.c_str());
	return true;
}

// Update message state in storage
bool MessageStore::update_message_state(const Bytes& message_hash, Type::Message::State state) {
	if (!_initialized) {
		ERROR("MessageStore not initialized");
		return false;
	}

	std::string message_path = get_message_path(message_hash);

	if (!recover_message_payload(message_path, message_hash)) {
		if (recover_archived_message_payload(message_hash)) {
			return update_archived_message_state(message_hash, state);
		}
		WARNING("Message file not found: " + message_path);
		return false;
	}

	try {
		Bytes data;
		if (Utilities::OS::read_file(message_path.c_str(), data) == 0) {
			ERROR("Failed to read message file: " + message_path);
			return false;
		}

		_json_doc.clear();
		DeserializationError error = deserializeJson(_json_doc, data.data(), data.size());
		if (error) {
			ERROR("Failed to parse message file: " + std::string(error.c_str()));
			return false;
		}
		const char* stored_hash = _json_doc["hash"];
		if (!stored_hash || message_hash.toHex() != stored_hash) {
			ERROR("Message state update rejected due to filename collision");
			return false;
		}
		_json_doc["state"] = static_cast<int>(state);

		std::string json_str;
		serializeJson(_json_doc, json_str);
		Bytes replacement((const uint8_t*)json_str.data(), json_str.size());
		size_t extension_pos = message_path.rfind('.');
		std::string stem = message_path.substr(0, extension_pos);
		std::string temp_path = stem + ".tmp";
		std::string backup_path = stem + ".bak";
		Utilities::OS::remove_file(temp_path.c_str());
		Utilities::OS::remove_file(backup_path.c_str());
		if (Utilities::OS::write_file(temp_path.c_str(), replacement) != replacement.size()) {
			ERROR("Failed to write temporary message-state payload");
			Utilities::OS::remove_file(temp_path.c_str());
			return false;
		}
		Bytes verify;
		if (Utilities::OS::read_file(temp_path.c_str(), verify) != replacement.size() ||
		    verify.size() != replacement.size()) {
			ERROR("Failed to verify temporary message-state payload");
			Utilities::OS::remove_file(temp_path.c_str());
			return false;
		}
		_json_doc.clear();
		if (deserializeJson(_json_doc, verify.data(), verify.size()) ||
		    (_json_doc["state"] | -1) != static_cast<int>(state)) {
			ERROR("Temporary message-state payload failed validation");
			Utilities::OS::remove_file(temp_path.c_str());
			return false;
		}
		if (!Utilities::OS::rename_file(message_path.c_str(), backup_path.c_str())) {
			ERROR("Failed to preserve previous message-state payload");
			Utilities::OS::remove_file(temp_path.c_str());
			return false;
		}
		if (!Utilities::OS::rename_file(temp_path.c_str(), message_path.c_str())) {
			ERROR("Failed to commit message-state payload");
			Utilities::OS::rename_file(backup_path.c_str(), message_path.c_str());
			Utilities::OS::remove_file(temp_path.c_str());
			return false;
		}
		Utilities::OS::remove_file(backup_path.c_str());

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

	// Update conversation index - remove from all conversations
	ConversationInfo* changed_conversation = nullptr;
	for (size_t i = 0; i < MAX_CONVERSATIONS; ++i) {
		ConversationSlot& slot = _conversations_pool[i];
		if (!slot.in_use) {
			continue;
		}

		ConversationInfo& conv = slot.info;
		if (conv.has_message(message_hash)) {
			_transaction_snapshot = conv;
			if (!conv.remove_message_hash(message_hash)) {
				ERROR("Failed to remove message from in-memory conversation");
				return false;
			}
			changed_conversation = &conv;
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

	// Commit the logical deletion before removing payloads. If this fails,
	// retain both the old in-memory index and all message data.
	if (changed_conversation && !save_index()) {
		*changed_conversation = _transaction_snapshot;
		ERROR("Message deletion index commit failed");
		return false;
	}

	// The durable index no longer references this message. Payload cleanup is
	// now best-effort: a lingering blob is harmless, while deleting it before
	// the index commit would create a stale entry after reboot.
	std::string message_path = get_message_path(message_hash);
	if (Utilities::OS::file_exists(message_path.c_str()) &&
	    !Utilities::OS::remove_file(message_path.c_str())) {
		WARNING("Failed to remove unreferenced message file: " + message_path);
	}
	if (_archive_fs) {
		std::string arch_path = get_archive_message_path(message_hash);
		if (_archive_fs.exists(arch_path.c_str()) && !_archive_fs.remove(arch_path.c_str())) {
			WARNING("Failed to remove unreferenced archived message file: " + arch_path);
		}
	}
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
	_transaction_snapshot = slot->info;

	// Commit the logical deletion first. Restore the in-memory slot when the
	// transactional index write fails, leaving every payload intact.
	slot->clear();
	if (!save_index()) {
		slot->in_use = true;
		slot->set_peer_hash(peer_hash);
		slot->info = _transaction_snapshot;
		ERROR("Conversation deletion index commit failed");
		return false;
	}

	// Delete all now-unreferenced message files from BOTH hot and archive.
	for (size_t i = 0; i < _transaction_snapshot.message_count; ++i) {
		Bytes msg_hash = _transaction_snapshot.message_hash_bytes(i);
		std::string message_path = get_message_path(msg_hash);
		if (Utilities::OS::file_exists(message_path.c_str())) {
			if (!Utilities::OS::remove_file(message_path.c_str())) {
				WARNING("Failed to remove unreferenced message file: " + message_path);
			}
		}
		if (_archive_fs) {
			std::string arch_path = get_archive_message_path(msg_hash);
			if (_archive_fs.exists(arch_path.c_str())) {
				if (!_archive_fs.remove(arch_path.c_str())) {
					WARNING("Failed to remove unreferenced archived message file: " + arch_path);
				}
			}
		}
	}

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

	// Persist the empty index without first destroying the in-memory index.
	// This leaves all state recoverable when the transaction fails and lets us
	// enumerate payloads only after the logical clear is durable.
	if (!save_index(true)) {
		ERROR("Message store clear index commit failed");
		return false;
	}

	// Delete all now-unreferenced message files.
	for (size_t i = 0; i < MAX_CONVERSATIONS; ++i) {
		ConversationSlot& slot = _conversations_pool[i];
		if (!slot.in_use) {
			continue;
		}
		for (size_t j = 0; j < slot.info.message_count; ++j) {
			std::string message_path = get_message_path(slot.info.message_hash_bytes(j));
			if (Utilities::OS::file_exists(message_path.c_str())) {
				if (!Utilities::OS::remove_file(message_path.c_str())) {
					WARNING("Failed to remove unreferenced message file: " + message_path);
				}
			}
			if (_archive_fs) {
				std::string arch_path = get_archive_message_path(slot.info.message_hash_bytes(j));
				if (_archive_fs.exists(arch_path.c_str()) && !_archive_fs.remove(arch_path.c_str())) {
					WARNING("Failed to remove unreferenced archived message file: " + arch_path);
				}
			}
		}
		slot.clear();
	}

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
