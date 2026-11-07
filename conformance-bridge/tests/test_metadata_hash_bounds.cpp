#include "LXMF/MessageStore.cpp"
#include <stdexcept>

static void require(bool value) {
    if (!value) throw std::runtime_error("metadata hash bounds regression");
}
int main() {
    uint8_t bytes[LXMF::MESSAGE_HASH_SIZE + 1] = {1};
    RNS::Bytes valid(bytes, LXMF::MESSAGE_HASH_SIZE);
    meta_cache_insert(valid, "valid", 1.0, true, 0);
    MetadataCacheSlot* slot = nullptr;
    require(meta_cache_find(valid, &slot) && slot && slot->content_len == 5);
    for (size_t length : {size_t(0), size_t(1), LXMF::MESSAGE_HASH_SIZE - 1,
                          LXMF::MESSAGE_HASH_SIZE + 1}) {
        RNS::Bytes invalid(bytes, length);
        slot = reinterpret_cast<MetadataCacheSlot*>(1);
        require(!meta_cache_find(invalid, &slot) && !slot);
        meta_cache_insert(invalid, "invalid", 2.0, false, 1);
        require(meta_cache_find(valid, &slot) && slot->content_len == 5);
    }
}
