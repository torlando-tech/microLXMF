#pragma once

#include "LXMessage.h"
#include <microReticulum/Bytes.h>

#include <ArduinoJson.h>
#include <microStore/FileSystem.h>
#include <string>
#include <vector>

namespace LXMF {

	// Fixed pool sizes to eliminate heap fragmentation
	static constexpr size_t MAX_CONVERSATIONS = 32;
	static constexpr size_t MAX_MESSAGES_PER_CONVERSATION = 256;
	static constexpr size_t MESSAGE_HASH_SIZE = 32;  // SHA256 hash
	static constexpr size_t PEER_HASH_SIZE = 16;     // Truncated hash

	// Two-tier storage policy. The default `MessageStore` constructor
	// keeps everything on the main filesystem (the pre-tiered behavior),
	// which works fine until the partition fills up. Pyxis's LittleFS
	// partition is 1.875 MB and a sustained-receive soak can fill it
	// in ~30 min — at which point lfs_alloc panics with /0.
	//
	// To make the store sustainable, the consumer can supply a SECOND
	// filesystem via `set_archive_filesystem()` (eg microSD on T-Deck).
	// When set, save_message cull-walks each conversation after every
	// save: messages older than HOT_MESSAGES_PER_CONVERSATION are
	// MOVED (copy + delete) from the primary filesystem to the archive
	// filesystem. load_message tries primary first, then archive.
	//
	// The conversation's full hash list (up to MAX_MESSAGES_PER_CONVERSATION)
	// stays in the in-memory index either way, so scrollback past the
	// hot count just hits the archive. If the hash list itself fills,
	// add_message_hash evicts the oldest entirely (and deletes its
	// archive file too) — a hard cap.
	//
	// Without an archive filesystem set, cull just deletes — bounded
	// in-flash storage, but no historical scrollback.
	static constexpr size_t HOT_MESSAGES_PER_CONVERSATION = 50;

	/**
	 * @brief Message persistence and conversation management for LXMF
	 *
	 * Stores LXMF messages on the filesystem organized by conversation (peer).
	 * Maintains an index of conversations and message order for efficient retrieval.
	 *
	 * Storage structure:
	 *   <base_path>/
	 *     conversations.json         - Conversation index
	 *     messages/<hash>.json       - Individual message files
	 *     conversations/<peer_hash>/ - Per-conversation metadata
	 *
	 * Usage:
	 *   MessageStore store("/path/to/storage");
	 *   store.save_message(message);
	 *
	 *   auto conversations = store.get_conversations();
	 *   auto messages = store.get_messages_for_conversation(peer_hash);
	 *   LXMessage msg = store.load_message(message_hash);
	 */
	class MessageStore {

	public:
		/**
		 * @brief Conversation metadata with fixed-size message hash storage
		 */
		// Cache of the peer's last-known LXMF display name. The
		// authoritative source is Identity::recall_app_data(peer_hash),
		// which is in-memory only and lost on reboot — without this
		// cache the conversation list falls back to truncated hashes
		// every cold start until the peer re-announces.
		static constexpr size_t MAX_DISPLAY_NAME_LEN = 47;  // 47 + nul = 48

		// Cached preview of the conversation's last message. The
		// conversation list shows the last 30 chars of the newest message;
		// without this cache every list refresh opens + JSON-parses the
		// newest message file per conversation (LittleFS open + read +
		// deserialize dominate the load path on SPI flash). The cache is
		// written when the last message is stored, persisted in the
		// conversation index, and cleared whenever the last message is
		// replaced (delete, hard-cap eviction, out-of-order save).
		// preview_valid is the "cache is populated" signal (NOT a non-empty
		// string): empty-content messages (location shares, blank pings)
		// also get cached — as an empty preview with preview_valid=true —
		// so callers never re-read the message file for a tail they have
		// already seen. preview_valid=false means "no cached preview" and
		// callers fall back to reading the message file (old index
		// generations, or the one-shot warm-up after a delete/clear).
		static constexpr size_t MAX_LAST_PREVIEW_LEN = 47;  // 47 + nul = 48

		struct ConversationInfo {
			// Fixed arrays eliminate ~6KB Bytes metadata overhead per conversation
			// (256 messages × 24 bytes metadata = 6.1KB saved per conversation)
			uint8_t peer_hash[PEER_HASH_SIZE];
			uint8_t message_hashes[MAX_MESSAGES_PER_CONVERSATION][MESSAGE_HASH_SIZE];
			size_t message_count = 0;          // Number of messages in this conversation
			double last_activity = 0.0;        // Timestamp of most recent message
			size_t unread_count = 0;           // Number of unread messages
			uint8_t last_message_hash[MESSAGE_HASH_SIZE];
			char display_name[MAX_DISPLAY_NAME_LEN + 1] = {0};  // Last seen, nul-terminated
			// Preview of the last message's content (first 47 chars), kept in
			// sync with last_message_hash. preview_valid=true means the
			// preview reflects the current last message — including an
			// empty string for empty-content messages. preview_valid=false
			// (default, after clear()/delete/cull, or when the index
			// generation predates the field) means callers must fall back
			// to reading the message file.
			char last_preview[MAX_LAST_PREVIEW_LEN + 1] = {0};
			bool preview_valid = false;

			// Helper methods for accessing fixed arrays as Bytes
			RNS::Bytes peer_hash_bytes() const { return RNS::Bytes(peer_hash, PEER_HASH_SIZE); }
			RNS::Bytes message_hash_bytes(size_t idx) const {
				if (idx >= message_count) return RNS::Bytes();
				return RNS::Bytes(message_hashes[idx], MESSAGE_HASH_SIZE);
			}
			RNS::Bytes last_message_hash_bytes() const { return RNS::Bytes(last_message_hash, MESSAGE_HASH_SIZE); }

			void set_peer_hash(const RNS::Bytes& b) {
				size_t len = std::min(b.size(), PEER_HASH_SIZE);
				memcpy(peer_hash, b.data(), len);
				if (len < PEER_HASH_SIZE) memset(peer_hash + len, 0, PEER_HASH_SIZE - len);
			}
			void set_last_message_hash(const RNS::Bytes& b) {
				size_t len = std::min(b.size(), MESSAGE_HASH_SIZE);
				memcpy(last_message_hash, b.data(), len);
				if (len < MESSAGE_HASH_SIZE) memset(last_message_hash + len, 0, MESSAGE_HASH_SIZE - len);
			}
			bool peer_hash_equals(const RNS::Bytes& b) const {
				if (b.size() != PEER_HASH_SIZE) return false;
				return memcmp(peer_hash, b.data(), PEER_HASH_SIZE) == 0;
			}

			/**
			 * @brief Add a message hash to this conversation
			 * @param hash Message hash to add
			 * @return True if added, false if already exists or pool full
			 */
			bool add_message_hash(const RNS::Bytes& hash);

			/**
			 * @brief Check if conversation has a specific message
			 * @param hash Message hash to check
			 * @return True if message exists in this conversation
			 */
			bool has_message(const RNS::Bytes& hash) const;

			/**
			 * @brief Remove a message hash from this conversation
			 * @param hash Message hash to remove
			 * @return True if removed, false if not found
			 */
			bool remove_message_hash(const RNS::Bytes& hash);

			/**
			 * @brief Clear all data in this conversation info
			 */
			void clear();
		};

		/**
		 * @brief Fixed-size slot for conversation storage
		 */
		struct ConversationSlot {
			bool in_use = false;
			uint8_t peer_hash[PEER_HASH_SIZE];
			ConversationInfo info;

			// Helper methods
			RNS::Bytes peer_hash_bytes() const { return RNS::Bytes(peer_hash, PEER_HASH_SIZE); }
			void set_peer_hash(const RNS::Bytes& b) {
				size_t len = std::min(b.size(), PEER_HASH_SIZE);
				memcpy(peer_hash, b.data(), len);
				if (len < PEER_HASH_SIZE) memset(peer_hash + len, 0, PEER_HASH_SIZE - len);
			}
			bool peer_hash_equals(const RNS::Bytes& b) const {
				if (b.size() != PEER_HASH_SIZE) return false;
				return memcmp(peer_hash, b.data(), PEER_HASH_SIZE) == 0;
			}

			/**
			 * @brief Clear this slot and mark as not in use
			 */
			void clear();
		};

		/**
		 * @brief Lightweight message metadata for fast loading
		 *
		 * Contains only fields needed for chat list display, avoiding
		 * expensive msgpack unpacking.
		 */
		struct MessageMetadata {
			RNS::Bytes hash;
			std::string content;
			double timestamp;
			bool incoming;
			int state;  // Type::Message::State as int
			bool valid;  // True if loaded successfully
		};

	public:
		/**
		 * @brief Construct MessageStore
		 *
		 * @param base_path Base directory for message storage
		 */
		MessageStore(const std::string& base_path);

		~MessageStore();

	public:
		/**
		 * @brief Configure an archive filesystem for older messages
		 *
		 * When set, save_message cull-walks each conversation after every
		 * save: messages older than HOT_MESSAGES_PER_CONVERSATION are
		 * moved from the primary (hot) filesystem to this archive
		 * filesystem. load_message falls back to the archive on a miss.
		 *
		 * @param fs        Archive filesystem (eg microSD on T-Deck).
		 *                  Pass an empty filesystem (default-constructed)
		 *                  to disable archiving.
		 * @param base_path Subdirectory on the archive filesystem to use
		 *                  for the message store. Defaults to the same
		 *                  base_path used for the hot filesystem.
		 */
		void set_archive_filesystem(microStore::FileSystem fs, const std::string& base_path = "");

		/**
		 * @brief Whether an archive filesystem is configured
		 */
		bool has_archive() const;

		/**
		 * @brief Cache a peer's LXMF display name in the conversation
		 *        index. The name is persisted to conv.json so it
		 *        survives reboots — without this the conversation list
		 *        falls back to truncated hashes every cold start until
		 *        the peer re-announces.
		 *
		 * @param peer_hash    Peer's destination hash
		 * @param display_name Resolved display name (eg from
		 *                     LXMF::display_name_from_app_data on
		 *                     Identity::recall_app_data result)
		 * @return True if the name was stored / updated. No-op if the
		 *         conversation isn't in the pool yet, or if the cached
		 *         name is already identical.
		 */
		bool set_display_name(const RNS::Bytes& peer_hash,
		                      const std::string& display_name);

		/**
		 * @brief Read the cached display name for a peer.
		 * @return Empty string if none cached.
		 */
		std::string get_display_name(const RNS::Bytes& peer_hash) const;

		/**
		 * @brief Save a message to storage
		 *
		 * Saves the message and updates the conversation index.
		 * Messages are organized by peer (the other party in the conversation).
		 *
		 * @param message Message to save
		 * @return True if saved successfully
		 */
		bool save_message(const LXMessage& message);

		/**
		 * @brief Load a message from storage
		 *
		 * @param message_hash Hash of the message to load
		 * @return LXMessage object (or empty if not found)
		 */
		LXMessage load_message(const RNS::Bytes& message_hash);

		/**
		 * @brief Load only message metadata (fast path for chat list)
		 *
		 * Reads content/timestamp/state directly from JSON without msgpack unpacking.
		 * Much faster than load_message() for displaying message lists.
		 *
		 * @param message_hash Hash of the message to load
		 * @return MessageMetadata struct (check .valid field)
		 */
		MessageMetadata load_message_metadata(const RNS::Bytes& message_hash);

		/**
		 * @brief Update message state in storage
		 *
		 * Updates just the state field of a stored message.
		 *
		 * @param message_hash Hash of the message to update
		 * @param state New state value
		 * @return True if updated successfully
		 */
		bool update_message_state(const RNS::Bytes& message_hash, Type::Message::State state);

		/**
		 * @brief Delete a message from storage
		 *
		 * Removes the message file and updates the conversation index.
		 *
		 * @param message_hash Hash of the message to delete
		 * @return True if deleted successfully
		 */
		bool delete_message(const RNS::Bytes& message_hash);

		/**
		 * @brief Get list of all conversation peer hashes
		 *
		 * Returns peer hashes sorted by last activity (most recent first).
		 *
		 * @return Vector of peer hashes
		 */
		std::vector<RNS::Bytes> get_conversations();

		/**
		 * @brief Get conversation info for a peer
		 *
		 * @param peer_hash Hash of the peer
		 * @return ConversationInfo (or empty if not found)
		 */
		ConversationInfo get_conversation_info(const RNS::Bytes& peer_hash);

		/**
		 * @brief Get all message hashes for a conversation
		 *
		 * Returns messages in chronological order (oldest first).
		 *
		 * @param peer_hash Hash of the peer
		 * @return Vector of message hashes
		 */
		std::vector<RNS::Bytes> get_messages_for_conversation(const RNS::Bytes& peer_hash);

		/**
		 * @brief Get the last message hash for a conversation
		 *
		 * Returns the hash cached in the conversation index (maintained by
		 * save_message / delete_message / cull). Empty Bytes if the
		 * conversation doesn't exist or is empty. O(1), no filesystem I/O.
		 *
		 * @param peer_hash Hash of the peer
		 * @return Last message hash (empty if none)
		 */
		RNS::Bytes get_last_message_hash(const RNS::Bytes& peer_hash) const;

		/**
		 * @brief Get the unread message count for one conversation
		 *
		 * @param peer_hash Hash of the peer
		 * @return Unread count (0 if the conversation doesn't exist)
		 */
		size_t get_conversation_unread_count(const RNS::Bytes& peer_hash) const;

		/**
		 * @brief Get the cached preview of a conversation's last message
		 *
		 * Returns the first MAX_LAST_PREVIEW_LEN chars of the newest
		 * message's content (plus its timestamp) from the in-memory
		 * conversation index. O(1), no filesystem I/O — this is what the
		 * conversation list needs and it removes one LittleFS open + read
		 * + JSON parse per conversation from every list refresh.
		 *
		 * out_preview may be empty: that is a legitimate cached preview
		 * for an empty-content last message (location share, blank ping).
		 * Returns false (and leaves outputs untouched) only when no
		 * cached preview exists: no conversation, no last message, or the
		 * cache is unpopulated (old index generation, or a tail replaced
		 * since the last cache write). Callers fall back to
		 * load_message_metadata() in that case and should re-pop the
		 * cache with set_last_message_preview().
		 *
		 * @param peer_hash Hash of the peer
		 * @param out_preview Receives the cached preview (may be empty)
		 * @param out_timestamp Receives the last message's timestamp
		 * @return True when a cached preview is available
		 */
		bool get_last_message_preview(const RNS::Bytes& peer_hash,
		                              std::string& out_preview,
		                              double& out_timestamp) const;

		/**
		 * @brief Set (or clear, when preview is empty) the cached preview
		 *
		 * An empty preview is a valid cached state: it marks the current
		 * empty-content tail as "already read" so callers do not re-read
		 * the message file on every refresh. Maintained automatically by
		 * save_message / delete_message; also used by callers that
		 * warm the cache from a fallback read. Does NOT commit the index.
		 */
		void set_last_message_preview(const RNS::Bytes& peer_hash,
		                              const std::string& preview);

		/**
		 * @brief Persist the in-memory conversation index to disk
		 *
		 * Public commit point for callers that mutate the in-memory index
		 * through the cache accessors (e.g. set_last_message_preview from
		 * a fallback warm-up) and want the change to survive reboot
		 * without triggering a message save/delete of their own. Rewrites
		 * the whole index file atomically. Does NOT take any additional
		 * state; callers are responsible for not racing concurrent store
		 * writers (the UI drains this out-of-lock, like mark-read).
		 *
		 * @return True if the index was written successfully
		 */
		bool commit_index();

		/**
		 * @brief Mark all messages in conversation as read
		 *
		 * @param peer_hash Hash of the peer
		 */
		void mark_conversation_read(const RNS::Bytes& peer_hash);

		/**
		 * @brief Delete entire conversation
		 *
		 * Removes all messages and conversation metadata.
		 *
		 * @param peer_hash Hash of the peer
		 * @return True if deleted successfully
		 */
		bool delete_conversation(const RNS::Bytes& peer_hash);

		/**
		 * @brief Get total number of stored messages
		 *
		 * @return Message count
		 */
		size_t get_message_count() const;

		/**
		 * @brief Get total number of conversations
		 *
		 * @return Conversation count
		 */
		size_t get_conversation_count() const;

		/**
		 * @brief Get total unread message count across all conversations
		 *
		 * @return Unread message count
		 */
		size_t get_unread_count() const;

		/**
		 * @brief Clear all stored messages and conversations
		 *
		 * WARNING: This permanently deletes all data.
		 *
		 * @return True if cleared successfully
		 */
		bool clear_all();

	private:
		/**
		 * @brief Initialize storage directories
		 *
		 * Creates base_path, messages/, and conversations/ directories if needed.
		 *
		 * @return True if initialized successfully
		 */
		bool initialize_storage();

		/**
		 * @brief Load conversation index from disk
		 *
		 * Loads conversations.json into _conversations_pool.
		 */
		bool load_index();
		bool load_index_file(const std::string& index_path);

		/**
		 * @brief Save conversation index to disk
		 *
		 * Persists _conversations_pool to conversations.json.
		 *
		 * @return True if saved successfully
		 */
		bool save_index(bool empty = false);

		/**
		 * @brief Get filesystem path for a message file
		 *
		 * @param message_hash Hash of the message
		 * @return Full path to message JSON file
		 */
		std::string get_message_path(const RNS::Bytes& message_hash) const;
		bool recover_message_payload(const std::string& message_path,
		                             const RNS::Bytes& expected_hash);
		bool recover_archived_message_payload(const RNS::Bytes& expected_hash);

		/**
		 * @brief Get filesystem path for conversation directory
		 *
		 * @param peer_hash Hash of the peer
		 * @return Full path to conversation directory
		 */
		std::string get_conversation_path(const RNS::Bytes& peer_hash) const;

		/**
		 * @brief Determine peer hash from message
		 *
		 * For incoming messages: peer = source
		 * For outgoing messages: peer = destination
		 *
		 * @param message The message
		 * @param our_hash Our local identity hash
		 * @return Peer hash
		 */
		RNS::Bytes get_peer_hash(const LXMessage& message, const RNS::Bytes& our_hash) const;

		/**
		 * @brief Find a conversation slot by peer hash
		 *
		 * @param peer_hash Hash of the peer
		 * @return Pointer to ConversationSlot or nullptr if not found
		 */
		ConversationSlot* find_conversation(const RNS::Bytes& peer_hash);
		const ConversationSlot* find_conversation(const RNS::Bytes& peer_hash) const;

		/**
		 * @brief Get or create a conversation slot for a peer
		 *
		 * @param peer_hash Hash of the peer
		 * @return Pointer to ConversationSlot or nullptr if pool is full
		 */
		ConversationSlot* get_or_create_conversation(const RNS::Bytes& peer_hash);

		/**
		 * @brief Count number of active conversations in pool
		 *
		 * @return Number of in-use conversation slots
		 */
		size_t count_conversations() const;

	private:
		/**
		 * @brief Cull-walk a conversation, archiving messages older than
		 *        HOT_MESSAGES_PER_CONVERSATION.
		 *
		 * For each in-memory hash beyond the hot count, copy the file
		 * from the primary filesystem to the archive (if set) and
		 * delete the primary copy. If no archive is set, just delete.
		 * The hash stays in the in-memory list either way.
		 */
		void cull_conversation_to_hot(const RNS::Bytes& peer_hash);

		/**
		 * @brief Move a single message file from hot → archive.
		 * @return True if archive successful, false otherwise. Hot copy
		 *         is removed iff archive succeeded (or no archive_fs).
		 */
		bool archive_one_message(const RNS::Bytes& message_hash);
		bool update_archived_message_state(const RNS::Bytes& message_hash,
		                                   Type::Message::State state);

		/**
		 * @brief Build the archive-side path for a message hash.
		 */
		std::string get_archive_message_path(const RNS::Bytes& message_hash) const;

		/**
		 * @brief Read a file from the archive filesystem.
		 * @return Number of bytes read; 0 on miss / no archive.
		 *
		 * Not const because microStore::FileSystem's accessors are
		 * non-const (they assert _impl and forward through a shared_ptr).
		 */
		size_t read_archive_file(const char* path, RNS::Bytes& out);

		/**
		 * @brief Write a file to the archive filesystem.
		 * @return Number of bytes written; 0 on failure / no archive.
		 */
		size_t write_archive_file(const char* path, const RNS::Bytes& data);

	private:
		std::string _base_path;
		ConversationSlot _conversations_pool[MAX_CONVERSATIONS];
		bool _initialized;

		// Optional archive filesystem (eg microSD). When `_archive_fs`
		// is truthy, save_message cull-walks each conversation and
		// older messages flow to `<_archive_path>/messages/<hash>.json`.
		microStore::FileSystem _archive_fs;
		std::string _archive_path;

		// Reusable JSON document to reduce heap fragmentation
		// Note: This class is assumed to be used from a single thread (main loop).
		// If called from multiple threads, this would need per-thread documents or locking.
		JsonDocument _json_doc;

		// Reusable rollback snapshot for save_message(). ConversationInfo is
		// larger than 8 KiB, so placing it in the save_message stack frame
		// overflows callers such as Pyxis's LVGL task. MessageStore already has
		// a single-threaded contract because _json_doc is shared, so reuse one
		// object-owned snapshot instead of allocating or copying it on the stack.
		ConversationInfo _transaction_snapshot;
	};

}  // namespace LXMF
