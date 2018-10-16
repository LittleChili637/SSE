//
//  janus_server.cpp
//  sophos
//
//  Created by Raphael Bost on 06/06/2017.
//  Copyright © 2017 Raphael Bost. All rights reserved.
//

#include "janus_server.hpp"

#include <set>

namespace sse {
    namespace sophos {
        
        using namespace janus;
        
        template<>
        struct serialization<JanusServer::cached_result_type>
        {
            std::string serialize(const JanusServer::cached_result_type& elt)
            {
                logger::log(logger::DBG) << "Serializing pair (" << hex_string(elt.first) << ", " << hex_string(elt.second) << ")\n";
               
                return std::string((char*)(&(elt.first)), sizeof(index_type)) + std::string(elt.second.begin(), elt.second.end());;
            }
            bool deserialize(std::string::iterator& begin, const std::string::iterator& end, JanusServer::cached_result_type& out)
            {

                if (((unsigned int)(end-begin)) < sizeof(janus::index_type)+sizeof(crypto::punct::kTagSize)) {
                    
                    if (end != begin) {
                        logger::log(logger::ERROR) << "Unable to deserialize" << std::endl;
                    }

                    return false;
                }
                logger::log(logger::DBG) << "Deserialized string: " << hex_string(std::string(begin, begin+sizeof(janus::index_type)+sizeof(crypto::punct::kTagSize))) << "\n";

                index_type ind;
                crypto::punct::tag_type tag;

                std::copy(begin, begin+sizeof(index_type), &ind);
                std::copy(begin+sizeof(index_type), begin+sizeof(index_type)+tag.size(), tag.begin());
                
                begin +=sizeof(index_type)+tag.size();
                
                out = std::make_pair(ind, std::move(tag));
                
                logger::log(logger::DBG) << "Deserializing pair (" << hex_string(out.first) << ", " << hex_string(out.second) << ")\n";

                return true;
            }
        };
 
    }
}
namespace sse {
    namespace janus {
        
        
        
        
//        static inline std::string insertion_db_path(const std::string &path)
//        {
//            return path + "/insertion.db";
//        }
//        
//        static inline std::string deletion_db_path(const std::string &path)
//        {
//            return path + "/deletion.db";
//        }
        
        JanusServer::JanusServer(const std::string& db_add_path, const std::string& db_del_path, const std::string& db_cache_path,bool usehdd) :
            insertion_server_(db_add_path,usehdd), deletion_server_(db_del_path,usehdd), cached_results_edb_(db_cache_path)
        {}

        
        
        
        std::list<index_type> JanusServer::search(const SearchRequest& req)
        {
//            std::list<crypto::punct::ciphertext_type> insertions = insertion_server_.search(req.insertion_search_request, true);
//
//            std::list<crypto::punct::key_share_type> key_shares = deletion_server_.search(req.deletion_search_request, true);
            
            std::list<crypto::punct::ciphertext_type> insertions = insertion_server_.search_simple_parallel(req.insertion_search_request, 8, true);
            
            std::list<crypto::punct::key_share_type> key_shares = deletion_server_.search_simple_parallel(req.deletion_search_request, 8, true);
            
            key_shares.push_front(req.first_key_share);
            
            // construct a set of newly removed tags
            std::set<crypto::punct::tag_type> removed_tags;
            auto sk_it = key_shares.begin();
            ++sk_it; // skip the first element
            for ( ; sk_it != key_shares.end(); ++sk_it){
                auto tag = crypto::punct::extract_tag(*sk_it);
                logger::log(logger::DBG) << "tag " << hex_string(tag) << " is removed" << std::endl;
                removed_tags.insert(std::move(tag));
            }
            
            crypto::PuncturableDecryption decryptor(
                                                    crypto::punct::punctured_key_type{
                                                        std::make_move_iterator(std::begin(key_shares)),
                                                        std::make_move_iterator(std::end(key_shares)) }
                                                    );
            
            
            std::list<index_type> results;
            std::list<cached_result_type> cached_res_list;
            
            // get previously cached elements
            cached_results_edb_.get(req.keyword_token, cached_res_list);
            
            
            // filter the previously cached elements to remove newly removed entries
            auto it = cached_res_list.begin();
            
            while (it != cached_res_list.end()) {
                if(removed_tags.count(it->second) > 0)
                {
                    it = cached_res_list.erase(it);
                }else{
                    results.push_back(it->first);
                    ++it;
                }
            }
            
            
            for (auto ct : insertions)
            {
                index_type r;
                if (decryptor.decrypt(ct, r)) {
                    results.push_back(r);
                    cached_res_list.push_back(std::make_pair(r,crypto::punct::extract_tag(ct)));
                }
            }
            
            // store results in the cache
            cached_results_edb_.put(req.keyword_token, cached_res_list);
            
            
            return results;
        }
        
        std::list<index_type> JanusServer::search_parallel(const SearchRequest& req, uint8_t threads_count)
        {
            assert(threads_count > 1);
            
            
            // use one result list per thread so to avoid using locks
            std::list<index_type> *result_lists = new std::list<index_type>[threads_count];
            
            auto callback = [&result_lists](index_type i, uint8_t thread_id)
            {
                result_lists[thread_id].push_back(i);
            };
            
            search_parallel(req, threads_count, callback);
            
            // merge the result lists
            std::list<index_type> results(std::move(result_lists[0]));
            for (uint8_t i = 1; i < threads_count; i++) {
                results.splice(results.end(), result_lists[i]);
            }
            
            delete []  result_lists;
            
            return results;
        }

        void JanusServer::search_parallel(const SearchRequest& req, uint8_t threads_count, const std::function<void(index_type)> &post_callback)
        {
            auto aux = [&post_callback](index_type ind, uint8_t i)
            {
                post_callback(ind);
            };
            search_parallel(req, threads_count, aux);
        }
        
        void JanusServer::search_parallel(const SearchRequest& req, uint8_t threads_count, const std::function<void(index_type, uint8_t)> &post_callback)
        {
            // start by retrieving the key shares
            std::list<crypto::punct::key_share_type> key_shares = deletion_server_.search_simple_parallel(req.deletion_search_request, threads_count, true);

            key_shares.push_front(req.first_key_share);

            
            
            
            
            crypto::PuncturableDecryption decryptor(
                                                    crypto::punct::punctured_key_type{
                                                        std::make_move_iterator(std::begin(key_shares)),
                                                        std::make_move_iterator(std::end(key_shares)) }
                                                    );
            
            std::list<cached_result_type> new_cache, filtered_cache;
            std::mutex cache_mtx;
            
            auto decryption_callback = [this, &req, &decryptor, &post_callback, &new_cache, &cache_mtx](crypto::punct::ciphertext_type ct, uint8_t i)
            {
                index_type r;
                if (decryptor.decrypt(ct, r)) {
                    post_callback(r, i);
                    
                    cache_mtx.lock();
                    new_cache.push_back(std::make_pair(r,crypto::punct::extract_tag(ct)));
                    cache_mtx.unlock();
                }

            };
            
            auto decryption_callback_unique = [&decryption_callback](crypto::punct::ciphertext_type ct)
            {
                decryption_callback(ct, 1);
            };
            
            // this job will be used to retrieve cached results and filter them
            auto cached_res_job = [this, &req, &key_shares, &post_callback, &threads_count, &filtered_cache]()
            {

                // construct a set of newly removed tags
                std::set<crypto::punct::tag_type> removed_tags;
                auto sk_it = key_shares.begin();
                ++sk_it; // skip the first element
                for ( ; sk_it != key_shares.end(); ++sk_it){
                    auto tag = crypto::punct::extract_tag(*sk_it);
                    logger::log(logger::DBG) << "tag " << hex_string(tag) << " is removed" << std::endl;
                    removed_tags.insert(std::move(tag));
                }
                
                
                // get previously cached elements
                cached_results_edb_.get(req.keyword_token, filtered_cache);
                
                // filter the previously cached elements to remove newly removed entries
                // filter the previously cached elements to remove newly removed entries
                auto it = filtered_cache.begin();
                
                while (it != filtered_cache.end()) {
                    if(removed_tags.count(it->second) > 0)
                    {
                        it = filtered_cache.erase(it);
                    }else{
                        post_callback(it->first, threads_count-1); // this job has id threads_count-1
                        ++it;
                    }
                }
                
            };
            
            
            // start the cached result job
            std::thread cache_thread = std::thread(cached_res_job);

            
            // wait for the cache thread to finish
            cache_thread.join();

            // run the search on the insertion SE with the decryption_callback
            // we have to start the cache_thread first because the next call is blocking
//            insertion_server_.search_simple_parallel(req.insertion_search_request, decryption_callback, threads_count-1); // one thread is already in use
            insertion_server_.search(req.insertion_search_request, decryption_callback_unique);
            
            // merge the new result list with the filtered cache
            new_cache.splice(new_cache.end(), filtered_cache);

            // store results in the cache
            cached_results_edb_.put(req.keyword_token, new_cache);
        }
        
        
        
        void JanusServer::insert_entry(const InsertionRequest& req)
        {
            insertion_server_.update(req);
        }
        
        void JanusServer::delete_entry(const DeletionRequest& req)
        {
            deletion_server_.update(req);
        }

        std::ostream& JanusServer::print_stats(std::ostream& out) const
        {
            insertion_server_.print_stats(out);
            deletion_server_.print_stats(out);
            return out;
        }

        void JanusServer::flush_edb()
        {
            insertion_server_.flush_edb();
            deletion_server_.flush_edb();
        }


    }
}
