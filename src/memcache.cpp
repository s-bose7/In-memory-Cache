#ifndef MEM_CACHE_CPP
#define MEM_CACHE_CPP

#include "../include/memcache.h"


template<typename K, typename V>
MemCache<K, V>::MemCache(int capacity) {
    // Initialize this cache with given capacity
    this->MAX_SIZE = capacity;
    this->curr_size = 0;
    this->HEAD = new FrequencyNode<KeyNode<K>>();
    this->thread_ttl = thread(&MemCache::run_ttl_thread, this);
}


template<typename K, typename V>
MemCache<K, V>::~MemCache(){
    lock_guard<mutex> lock(cache_mutex);
    this->stop_t = true;
    this->thread_ttl.join();
}


template<typename K, typename V>
K MemCache<K, V>::get(K key) {
    if (exists(key)){
        MapItem<K, V, KeyNode<K>> map_item = bykey.at(key);
        update_frequency_of_the(key);
        return map_item.value;
    }
    return -1;
}


template<typename K, typename V>
void MemCache<K, V>::update_frequency_of_the(K key){
    MapItem<K, V, KeyNode<K>> map_item = bykey.at(key);
    
    FrequencyNode<KeyNode<K>> *cur_freq = map_item.parent;
    FrequencyNode<KeyNode<K>> *new_freq = cur_freq->next;

    if(new_freq->frequency != cur_freq->frequency + 1){
        new_freq = get_new_frequency_node(
            cur_freq->frequency + 1, 
            cur_freq, 
            new_freq
        );
    }

    bykey.at(key).parent = new_freq;

    KeyNode<K> *keynode_to_shift = map_item.node;
    remove_keynode_from_frequencynode(cur_freq, keynode_to_shift);
    put_keynode_in_frequencynode(new_freq, keynode_to_shift);
}


template<typename K, typename V>
bool MemCache<K, V>::exists(K key) {
    if(bykey.count(key) == 0){
        return false;
    }
    return true;
}


template<typename K, typename V>
void MemCache<K, V>::put(K key, V value, unsigned long ttl) {
    if(ttl > 0){
        expiration_map[key] = steady_clock::now() + chrono::seconds(ttl);
    }
    if(exists(key)){
        // Update the value of the key 
        bykey.at(key).value = value;
        // Update the frequency of the key
        update_frequency_of_the(key);
        return;
    }
    if(this->curr_size == this->MAX_SIZE){
        apply_eviction_policy();
    }
    FrequencyNode<KeyNode<K>> *freq_node = HEAD->next;
    if(freq_node->frequency != 1){
        freq_node = get_new_frequency_node(1, HEAD, freq_node);
    }
    KeyNode<K> *key_node = new KeyNode<K>(key);
    put_keynode_in_frequencynode(freq_node, key_node);
    // Put a new entry into the Hash Table
    bykey.insert(make_pair(key, MapItem<K, V, KeyNode<K>>(value, freq_node, key_node)));
    ++this->curr_size;
}


template<typename K, typename V>
void MemCache<K, V>::apply_eviction_policy() {
    
    int remove_key;
    FrequencyNode<KeyNode<K>> *LFUNode = HEAD->next;
    
    KeyNode<K>* MRUNode = LFUNode->mrukeynode; 
    KeyNode<K>* LRUNode = LFUNode->lrukeynode;
    KeyNode<K>* node_to_remove = nullptr;

    if(LFUNode->local_keys_length > 1){
        remove_key = LRUNode->key;
    }else{
        remove_key = MRUNode->key; 
    }
    lock_guard<mutex> lock(cache_mutex);
    remove(remove_key);
    if(expiration_map.count(remove_key) > 0){
        // If not already removed by expiration_thread
        expiration_map.erase(remove_key);
    }
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
    new_parent->local_keys_length++;
    if(!new_parent->mrukeynode && !new_parent->lrukeynode){
        new_parent->mrukeynode = new_parent->lrukeynode = child;
    }else{
        child->down = new_parent->mrukeynode;
        new_parent->mrukeynode->up = child;
        new_parent->mrukeynode = child;
    }
}


template<typename K, typename V>
void MemCache<K, V>::remove_keynode_from_frequencynode(
    FrequencyNode<KeyNode<K>>* parent, 
    KeyNode<K>* child
){
    if(parent->local_keys_length == 1) {
        // If the current frequency has only one key: 
        // Remove it and delete the frequency node
        parent->prev->next = parent->next;
        parent->next->prev = parent->prev;
        delete parent;

    }else{
        // If the current frequency has more than one key: 
        // Remove the key node from the current frequency.
        if (child == parent->mrukeynode) {
            parent->mrukeynode = parent->mrukeynode->down;
        } else if (child == parent->lrukeynode) {
            parent->lrukeynode = parent->lrukeynode->up;
        } else {
            child->up->down = child->down;
            child->down->up = child->up;
        }
        parent->local_keys_length--;
    }
    child->up = child->down = nullptr;
}


template<typename K, typename V>
bool MemCache<K, V>::remove(K key) {
    bool key_removal_status = false;
    if(exists(key)){
        MapItem<K, V, KeyNode<K>> map_item = bykey.at(key);
        remove_keynode_from_frequencynode(
            map_item.parent, 
            map_item.node
        );
        bykey.erase(key);
        --this->curr_size;
        key_removal_status = true;
    }
    return key_removal_status;
}


template<typename K, typename V>
void MemCache<K, V>::run_ttl_thread(){
    int sleep_t = 5;
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
    for(auto iter=expiration_map.begin(); iter!=expiration_map.end();){
        if(iter->second <= now){
            remove(iter->first);
            iter = expiration_map.erase(iter);
        }else{
            ++iter;
        }
    }
}


template<typename K, typename V>
bool MemCache<K, V>::clear(){
    std::lock_guard<std::mutex> lock(mutex);
    // Redifne everything
    HEAD = new FrequencyNode<KeyNode<K>>(); 
    curr_size = 0;
    bykey.clear();
    expiration_map.clear();
    return true;
}


template<typename K, typename V>
void MemCache<K, V>::resize(size_t new_capacity) {
    
    size_t available_ram = get_available_memory();
    cout<<available_ram<<endl;
    size_t required_ram = available_ram * sizeof(MapItem<K, V, KeyNode<K>>);
    cout<<required_ram<<endl;
    if(required_ram > available_ram){
        cerr << "Error: Not enough memory for capacity " << new_capacity << endl;
        return;
    }

    MAX_SIZE = new_capacity;
    while(curr_size > MAX_SIZE){
        apply_eviction_policy();
    }
}

#endif