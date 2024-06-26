#ifndef MEM_CACHE_CPP
#define MEM_CACHE_CPP

#include "../include/memcache.h"


template<typename K, typename V>
MemCache<K, V>::MemCache(int capacity) {
    // Initialize this cache with given capacity
    this->MAX_SIZE = capacity;
    this->curr_size = 0;
    this->HEAD = new FrequencyNode<KeyNode<K>>();
    this->thread_ttl = thread(&MemCache<K, V>::run_ttl_thread, this);
}


template<typename K, typename V>
MemCache<K, V>::~MemCache(){
    lock_guard<mutex> lock(cache_mutex);
    this->stop_t = true;
    this->thread_ttl.join();
}


template<typename K, typename V>
V MemCache<K, V>::get(K key) {
    lock_guard<mutex> lock(cache_mutex);

    if (bykey.count(key) > 0){
        MapItem<KeyNode<K>, string> map_item = bykey.at(key);
        update_frequency_of_the(key);
        // Return after decompressing the value
        return deserialize(Compressor::uncompress(map_item.value)); 
    }
    return V();
}


template<typename K, typename V>
void MemCache<K, V>::update_frequency_of_the(K key){
    MapItem<KeyNode<K>, string> map_item = bykey.at(key);
    
    FrequencyNode<KeyNode<K>> *cur_freq = map_item.node->parent;
    FrequencyNode<KeyNode<K>> *new_freq = cur_freq->next;

    if(new_freq->frequency != cur_freq->frequency + 1){
        new_freq = get_new_frequency_node(
            cur_freq->frequency + 1, 
            cur_freq, 
            new_freq
        );
    }

    bykey.at(key).node->parent = new_freq;

    KeyNode<K> *keynode_to_shift = map_item.node;
    remove_keynode_from_frequencynode(cur_freq, keynode_to_shift);
    put_keynode_in_frequencynode(new_freq, keynode_to_shift);
}


template<typename K, typename V>
bool MemCache<K, V>::exists(K key) {
    lock_guard<mutex> lock(cache_mutex);
    return bykey.count(key) > 0;
}


template<typename K, typename V>
unsigned int MemCache<K, V>::size(){
    lock_guard<mutex> lock(cache_mutex);
    return curr_size;
}

template<typename K, typename V>
string MemCache<K, V>::serialize(const V& val){
    if constexpr (std::is_arithmetic<V>::value) {
        // For arithmetic data types
        return Serializer::serialize<V>(val, std::true_type());
    } else if constexpr (std::is_same<V, string>::value) {
        // For string data types
        return Serializer::serialize(val);
    } else {
        // For custom object types i.e. user-defined types
        return Serializer::serialize<V>(val, std::false_type());
    }
}

template<typename K, typename V>
V MemCache<K, V>::deserialize(const string& serialized_val){
    if constexpr (std::is_arithmetic<V>::value) {
        // For arithmetic data types
        return Serializer::deserialize<V>(serialized_val, std::true_type());
    } else if constexpr (std::is_same<V, string>::value) {
        // For string data types
        return Serializer::deserialize(serialized_val);
    } else {
        // For custom object types i.e. user-defined types
        return Serializer::deserialize<V>(serialized_val, std::false_type());
    }
}


template<typename K, typename V>
void MemCache<K, V>::put(K key, V value, unsigned long ttl) {
    lock_guard<mutex> lock(cache_mutex);

    if(ttl < 1){
        ttl = INT_MAX; // Default ttl value
    }
    // Compress value
    string compressed_val = Compressor::compress(serialize(value));
    expiration_map[key] = steady_clock::now() + chrono::seconds(ttl);
    if(bykey.count(key) > 0) {
        // Update the value of the key 
        bykey.at(key).value = compressed_val;
        // Update the frequency of the key
        update_frequency_of_the(key);
        return;
    }
    if(curr_size == MAX_SIZE){
        apply_eviction_policy();
    }
    FrequencyNode<KeyNode<K>> *freq_node = HEAD->next;
    if(freq_node->frequency != 1){
        freq_node = get_new_frequency_node(1, HEAD, freq_node);
    }
    KeyNode<K> *key_node = new KeyNode<K>(key, freq_node);
    put_keynode_in_frequencynode(freq_node, key_node);
    // Put a new entry into the Hash Table
    bykey.insert(make_pair(key, MapItem<KeyNode<K>, string>(compressed_val, key_node)));
    ++curr_size;
}


template<typename K, typename V>
void MemCache<K, V>::apply_eviction_policy() {
    
    K remove_key;
    FrequencyNode<KeyNode<K>> *LFUNode = HEAD->next;
    
    KeyNode<K>* MRUNode = LFUNode->keynode_mru; 
    KeyNode<K>* LRUNode = LFUNode->keynode_lru;
    KeyNode<K>* node_to_remove = nullptr;

    if(LFUNode->num_keys_local > 1){
        remove_key = LRUNode->key;
    }else{
        remove_key = MRUNode->key; 
    }
    remove(bykey.at(remove_key));
}


template<typename K, typename V>
FrequencyNode<KeyNode<K>>* MemCache<K, V>::get_new_frequency_node(
    int freq, 
    FrequencyNode<KeyNode<K>>* prev, 
    FrequencyNode<KeyNode<K>>* next
) {
    FrequencyNode<KeyNode<K>> *new_freq_node = new FrequencyNode<KeyNode<K>>();
    new_freq_node->frequency = freq;
    new_freq_node->prev = prev;
    prev->next = new_freq_node;
    
    if(prev == next){
        new_freq_node->next = new_freq_node;
    }else{
        new_freq_node->next = next;
        next->prev = new_freq_node;
    }

    return new_freq_node;
}


template<typename K, typename V>
void MemCache<K, V>::put_keynode_in_frequencynode(
    FrequencyNode<KeyNode<K>>* new_parent, 
    KeyNode<K>* child
) {
    new_parent->num_keys_local++;
    if(!new_parent->keynode_mru && !new_parent->keynode_lru){
        new_parent->keynode_mru = new_parent->keynode_lru = child;
    }else{
        child->down = new_parent->keynode_mru;
        new_parent->keynode_mru->up = child;
        new_parent->keynode_mru = child;
    }
}


template<typename K, typename V>
void MemCache<K, V>::remove_keynode_from_frequencynode(
    FrequencyNode<KeyNode<K>>* parent, 
    KeyNode<K>* child
){
    if(parent->num_keys_local == 1) {
        // If the current frequency has only one key: 
        // Remove it and delete the frequency node
        parent->prev->next = parent->next;
        parent->next->prev = parent->prev;
        delete parent;

    }else{
        // If the current frequency has more than one key: 
        // Remove the key node from the current frequency.
        if (child == parent->keynode_mru) {
            parent->keynode_mru = parent->keynode_mru->down;
        } else if (child == parent->keynode_lru) {
            parent->keynode_lru = parent->keynode_lru->up;
        } else {
            child->up->down = child->down;
            child->down->up = child->up;
        }
        parent->num_keys_local--;
    }
    child->up = child->down = nullptr;
}


template<typename K, typename V>
void MemCache<K, V>::remove(MapItem<KeyNode<K>, string> item){
    // Seems redundant at first glance, but nessearly to avoid recursive locking.
    K key = item.node->key;
    remove_keynode_from_frequencynode(item.node->parent, item.node);
    bykey.erase(key);
    curr_size -= 1;
}


template<typename K, typename V>
bool MemCache<K, V>::remove(K key) {
    lock_guard<mutex> lock(cache_mutex);
    
    bool key_removal_status = false;
    if(bykey.count(key) > 0){
        MapItem<KeyNode<K>, string> map_item = bykey.at(key);
        remove_keynode_from_frequencynode(map_item.node->parent, map_item.node);
        bykey.erase(key);
        curr_size -= 1;
        key_removal_status = true;
    }
    return key_removal_status;
}


template<typename K, typename V>
void MemCache<K, V>::run_ttl_thread(){
    int sleep_t = 1;
    while(!stop_t) {
        {
            lock_guard<mutex> lock(cache_mutex);
            apply_expiration_policy();
        }
        this_thread::sleep_for(chrono::seconds(sleep_t));
    }
}


template<typename K, typename V>
void MemCache<K, V>::apply_expiration_policy(){
    auto now = steady_clock::now();
    for(auto iter=expiration_map.begin(); iter!=expiration_map.end(); ){
        if(iter->second <= now) {
            if(bykey.count(iter->first) > 0) {
                remove(bykey.at(iter->first));
            } else {
                // Key already removed by eviction policy or the client;
                // Do nothing
            }
            iter = expiration_map.erase(iter);
        } else {
            // Increment the iter
            ++iter;
        }
    }
}


template<typename K, typename V>
bool MemCache<K, V>::clear(){
    lock_guard<mutex> lock(cache_mutex);

    // Redifne everything
    HEAD = new FrequencyNode<KeyNode<K>>(); 
    curr_size = 0;
    bykey.clear();
    expiration_map.clear();
    return true;
}
    

template<typename K, typename V>
size_t MemCache<K, V>::get_base_required_memory(size_t capacity) noexcept {
    
    size_t ptr_s = sizeof(void*);
    size_t keynode_s = sizeof(K) + 2*ptr_s; 
    size_t mapitem_s = sizeof(V) + 2*ptr_s;
    size_t frequencynode_s = 2*sizeof(int) + 4*ptr_s; 
    // Space for a pointer to the bucket array and space for handling collisions
    size_t map_s = 1.5 * ptr_s;

    // Base size of 1 KV entry in the cache
    size_t base_size = sizeof(K) + map_s + keynode_s + mapitem_s;

    // FrequencyNode may or maynot get created for every cache entry
    // Accounting for memory where frequencyNode get created around 50% of the cache entriesthe
    return ((capacity * (base_size + frequencynode_s)) + ((capacity / 2) * frequencynode_s));
}


template<typename K, typename V>
void MemCache<K, V>::resize(size_t new_capacity) {
    
    size_t meta_factor = 1024;
    size_t available_ram = get_available_memory();
    size_t required_ram =  get_base_required_memory(new_capacity);
    if(required_ram > available_ram + meta_factor){
        cerr << "Error: Not enough memory for capacity " << new_capacity << endl;
        return;
    }

    MAX_SIZE = new_capacity;
    // If shrinking needed, invalidate LFRU keys.
    while(curr_size > MAX_SIZE)
    {
        apply_eviction_policy();
    }
}

#endif