//
// Sophos - Forward Private Searchable Encryption
// Copyright (C) 2016 Raphael Bost
//
// This file is part of Sophos.
//
// Sophos is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as
// published by the Free Software Foundation, either version 3 of the
// License, or (at your option) any later version.
//
// Sophos is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with Sophos.  If not, see <http://www.gnu.org/licenses/>.
//


#include "sophos_server.hpp"


#include "utils/utils.hpp"
#include "utils/logger.hpp"
#include "utils/thread_pool.hpp"

#include <iostream>
#include <algorithm>

namespace sse {
    namespace sophos {
        using namespace std;

        SophosServer::SophosServer(const std::string& db_path, const std::string& tdp_pk, bool usehdd) :
        edb_(db_path), public_tdp_(tdp_pk, 2 * std::thread::hardware_concurrency()) {
            this->usehdd = usehdd;
        }

        SophosServer::SophosServer(const std::string& db_path, const size_t tm_setup_size, const std::string& tdp_pk, bool usehdd) :
        edb_(db_path), /*edb_(db_path, tm_setup_size),*/
        public_tdp_(tdp_pk, 2 * std::thread::hardware_concurrency()) {
            this->usehdd = usehdd;
        }

        const std::string SophosServer::public_key() const {
            return public_tdp_.public_key();
        }

        std::list<array<uint8_t, kUpdateTokenSize> > SophosServer::search(const SearchRequest& req, bool deleteItems) {
            std::list<array<uint8_t, kUpdateTokenSize> > results;

            search_token_type st = req.token;

            if (logger::severity() <= logger::DBG) {

                logger::log(logger::DBG) << "Search token: " << hex_string(req.token) << std::endl;

                logger::log(logger::DBG) << "Derivation key: " << hex_string(req.derivation_key) << std::endl;
            }

            for (size_t i = 0; i < req.add_count; i++) {
                std::string st_string(reinterpret_cast<char*> (st.data()), st.size());
                array<uint8_t, kUpdateTokenSize> r;
                update_token_type ut;
                std::array<uint8_t, kUpdateTokenSize> mask;
                gen_update_token_masks(req.derivation_key, st.data(), ut, mask);

                if (logger::severity() <= logger::DBG) {
                    logger::log(logger::DBG) << "Derived token: " << hex_string(ut) << std::endl;
                }

                bool found = false;
                if (usehdd) {
                    found = edb_.get(ut, r);
                } else {
                    if (curArray.count(ut) > 0) {
                        found = true;
                        r = curArray[ut];
                    }

                }
                if (found) {
                    if (deleteItems) {
                        if (usehdd) {
                            edb_.remove(ut);
                        } else {
                            curArray.erase(ut);
                        }
                    }
                    r = xor_mask(r, mask);
                    results.push_back(r);
                } else {
                    //logger::log(logger::ERROR) << "We were supposed to find something!" << std::endl;
                }

                st = public_tdp_.eval(st);
            }

            return results;
        }

        void SophosServer::search_callback(const SearchRequest& req, std::function<void(index_type) > post_callback) {
            search_token_type st = req.token;


            if (logger::severity() <= logger::DBG) {
                logger::log(logger::DBG) << "Search token: " << hex_string(req.token) << std::endl;

                logger::log(logger::DBG) << "Derivation key: " << hex_string(req.derivation_key) << std::endl;
            }

            for (size_t i = 0; i < req.add_count; i++) {
                std::string st_string(reinterpret_cast<char*> (st.data()), st.size());
                index_type r;
                update_token_type ut;
                std::array<uint8_t, kUpdateTokenSize> mask;
                gen_update_token_masks(req.derivation_key, st.data(), ut, mask);

                if (logger::severity() <= logger::DBG) {
                    logger::log(logger::DBG) << "Derived token: " << hex_string(ut) << std::endl;
                }

                bool found = false;
                if (usehdd) {
                    found = edb_.get(ut, r);
                } else {
                    if (curArray.count(ut) > 0) {
                        found = true;
                        //r = curArray[ut];
                    }

                }

                if (found) {
                    if (logger::severity() <= logger::DBG) {
                        logger::log(logger::DBG) << "Found: " << std::hex << r << std::endl;
                    }

                    r = xor_mask(r, mask);
                    post_callback(r);
                } else {
                    //logger::log(logger::ERROR) << "We were supposed to find something!" << std::endl;
                }

                st = public_tdp_.eval(st);
            }
        }

        std::list<index_type> SophosServer::search_parallel_full(const SearchRequest& req) {
            std::list<index_type> results;

            search_token_type st = req.token;

            auto derivation_prf = crypto::Prf<kUpdateTokenSize>(req.derivation_key);

            if (logger::severity() <= logger::DBG) {
                logger::log(logger::DBG) << "Search token: " << hex_string(req.token) << std::endl;

                logger::log(logger::DBG) << "Derivation key: " << hex_string(req.derivation_key) << std::endl;
            }

            ThreadPool prf_pool(1);
            ThreadPool token_map_pool(1);
            ThreadPool decrypt_pool(1);

            auto decrypt_job = [&derivation_prf, &results](const index_type r, const std::string & st_string) {
                index_type v = xor_mask(r, derivation_prf.prf(st_string + '1'));
                results.push_back(v);
            };

            auto lookup_job = [&derivation_prf, &decrypt_pool, &decrypt_job, this](const std::string& st_string, const update_token_type & token) {
                index_type r;

                if (logger::severity() <= logger::DBG) {
                    logger::log(logger::DBG) << "Derived token: " << hex_string(token) << std::endl;
                }

                bool found = false;
                if (usehdd) {
                    found = edb_.get(token, r);
                } else {
                    if (curArray.count(token) > 0) {
                        found = true;
//                        r = curArray[token];
                    }
                }

                if (found) {
                    if (logger::severity() <= logger::DBG) {
                        logger::log(logger::DBG) << "Found: " << std::hex << r << std::endl;
                    }

                    decrypt_pool.enqueue(decrypt_job, r, st_string);

                } else {
                    //logger::log(logger::ERROR) << "We were supposed to find something!" << std::endl;
                }

            };


            auto derive_job = [&derivation_prf, &token_map_pool, &lookup_job](const std::string & input_string) {
                update_token_type ut = derivation_prf.prf(input_string + '0');

                token_map_pool.enqueue(lookup_job, input_string, ut);

            };

            // the rsa job launched with input index,max computes all the RSA tokens of order i + kN up to max
            auto rsa_job = [this, &st, &derive_job, &prf_pool](const uint8_t index, const size_t max, const uint8_t N) {
                search_token_type local_st = st;
                if (index != 0) {
                    local_st = public_tdp_.eval(local_st, index);
                }

                if (index < max) {
                    // this is a valid search token, we have to derive it and do a lookup
                    std::string st_string(reinterpret_cast<char*> (local_st.data()), local_st.size());
                    prf_pool.enqueue(derive_job, st_string);
                }

                for (size_t i = index + N; i < max; i += N) {
                    local_st = public_tdp_.eval(local_st, N);

                    std::string st_string(reinterpret_cast<char*> (local_st.data()), local_st.size());
                    prf_pool.enqueue(derive_job, st_string);
                }
            };

            std::vector<std::thread> rsa_threads;

            unsigned n_threads = std::thread::hardware_concurrency() - 3;

            for (uint8_t t = 0; t < n_threads; t++) {
                rsa_threads.push_back(std::thread(rsa_job, t, req.add_count, n_threads));
            }

            for (uint8_t t = 0; t < n_threads; t++) {
                rsa_threads[t].join();
            }

            prf_pool.join();
            token_map_pool.join();


            return results;
        }

        std::list<index_type> SophosServer::search_parallel(const SearchRequest& req, uint8_t access_threads) {
            std::list<index_type> results;
            std::mutex res_mutex;

            search_token_type st = req.token;

            //    auto derivation_prf = crypto::Prf<kUpdateTokenSize>(req.derivation_key);

            if (logger::severity() <= logger::DBG) {
                logger::log(logger::DBG) << "Search token: " << hex_string(req.token) << std::endl;

                logger::log(logger::DBG) << "Derivation key: " << hex_string(req.derivation_key) << std::endl;
            }

            ThreadPool access_pool(access_threads);

            auto access_job = [&req, this, &results, &res_mutex](const std::string & st_string) {
                update_token_type token;
                std::array<uint8_t, kUpdateTokenSize> mask;
                gen_update_token_masks(req.derivation_key, (uint8_t *) st_string.data(), token, mask);

                index_type r;

                if (logger::severity() <= logger::DBG) {
                    logger::log(logger::DBG) << "Derived token: " << hex_string(token) << std::endl;
                }

                bool found = false;
                if (usehdd) {
                    found = edb_.get(token, r);
                } else {
                    if (curArray.count(token) > 0) {
                        found = true;
                        //r = curArray[token];
                    }
                }

                if (found) {
                    if (logger::severity() <= logger::DBG) {
                        logger::log(logger::DBG) << "Found: " << std::hex << r << std::endl;
                    }
                } else {
                    //logger::log(logger::ERROR) << "We were supposed to find something!" << std::endl;
                }

                index_type v = xor_mask(r, mask);

                res_mutex.lock();
                results.push_back(v);
                res_mutex.unlock();

            };



            // the rsa job launched with input index,max computes all the RSA tokens of order i + kN up to max
            auto rsa_job = [this, &st, &access_job, &access_pool](const uint8_t index, const size_t max, const uint8_t N) {
                search_token_type local_st = st;
                if (index != 0) {
                    local_st = public_tdp_.eval(local_st, index);
                }

                if (index < max) {
                    // this is a valid search token, we have to derive it and do a lookup
                    std::string st_string(reinterpret_cast<char*> (local_st.data()), local_st.size());
                    access_pool.enqueue(access_job, st_string);
                }

                for (size_t i = index + N; i < max; i += N) {
                    local_st = public_tdp_.eval(local_st, N);

                    std::string st_string(reinterpret_cast<char*> (local_st.data()), local_st.size());
                    access_pool.enqueue(access_job, st_string);

                }
            };

            std::vector<std::thread> rsa_threads;

            unsigned n_threads = std::thread::hardware_concurrency() - access_threads;

            for (uint8_t t = 0; t < n_threads; t++) {
                rsa_threads.push_back(std::thread(rsa_job, t, req.add_count, n_threads));
            }

            for (uint8_t t = 0; t < n_threads; t++) {
                rsa_threads[t].join();
            }

            access_pool.join();

            return results;
        }

        std::list<index_type> SophosServer::search_parallel_light(const SearchRequest& req, uint8_t thread_count) {
            search_token_type st = req.token;
            std::list<index_type> results;
            std::mutex res_mutex;

            //    auto derivation_prf = crypto::Prf<kUpdateTokenSize>(req.derivation_key);

            if (logger::severity() <= logger::DBG) {
                logger::log(logger::DBG) << "Search token: " << hex_string(req.token) << std::endl;

                logger::log(logger::DBG) << "Derivation key: " << hex_string(req.derivation_key) << std::endl;
            }

            auto derive_access = [&req, this, &results, &res_mutex](const search_token_type st, size_t i) {

                update_token_type token;
                std::array<uint8_t, kUpdateTokenSize> mask;
                gen_update_token_masks(req.derivation_key, st.data(), token, mask);


                index_type r;

                if (logger::severity() <= logger::DBG) {
                    logger::log(logger::DBG) << "Derived token: " << hex_string(token) << std::endl;
                }

                bool found = false;
                if (usehdd) {
                    found = edb_.get(token, r);
                } else {
                    if (curArray.count(token) > 0) {
                        found = true;
                        //r = curArray[token];
                    }
                }

                if (found) {
                    if (logger::severity() <= logger::DBG) {
                        logger::log(logger::DBG) << "Found: " << std::hex << r << std::endl;
                    }

                    index_type v = xor_mask(r, mask);

                    res_mutex.lock();
                    results.push_back(v);
                    res_mutex.unlock();

                } else {
                    //logger::log(logger::ERROR) << "We were supposed to find a value mapped to key " << hex_string(token);
                    //logger::log(logger::ERROR) << " (" << i << "-th derived key from search token " << hex_string(st) << ")" << std::endl;
                }

            };



            // the rsa job launched with input index,max computes all the RSA tokens of order i + kN up to max
            auto job = [this, &st, &derive_access](const uint8_t index, const size_t max, const uint8_t N) {
                search_token_type local_st = st;
                if (index != 0) {
                    local_st = public_tdp_.eval(local_st, index);
                }

                if (index < max) {
                    // this is a valid search token, we have to derive it and do a lookup

                    derive_access(local_st, index);
                }

                for (size_t i = index + N; i < max; i += N) {
                    local_st = public_tdp_.eval(local_st, N);

                    derive_access(local_st, index);
                }
            };

            std::vector<std::thread> rsa_threads;

            //    unsigned n_threads = std::thread::hardware_concurrency()-access_threads;

            for (uint8_t t = 0; t < thread_count; t++) {
                rsa_threads.push_back(std::thread(job, t, req.add_count, thread_count));
            }

            for (uint8_t t = 0; t < thread_count; t++) {
                rsa_threads[t].join();
            }

            return results;
        }

        void SophosServer::search_parallel_callback(const SearchRequest& req, std::function<void(index_type) > post_callback, uint8_t rsa_thread_count, uint8_t access_thread_count, uint8_t post_thread_count) {
            search_token_type st = req.token;

            //    auto derivation_prf = crypto::Prf<kUpdateTokenSize>(req.derivation_key);

            if (logger::severity() <= logger::DBG) {
                logger::log(logger::DBG) << "Search token: " << hex_string(req.token) << std::endl;

                logger::log(logger::DBG) << "Derivation key: " << hex_string(req.derivation_key) << std::endl;
            }

            ThreadPool access_pool(access_thread_count);
            ThreadPool post_pool(post_thread_count);

            auto access_job = [&req, this, &post_pool, &post_callback](const search_token_type st, size_t i) {
                update_token_type token;
                std::array<uint8_t, kUpdateTokenSize> mask;
                gen_update_token_masks(req.derivation_key, st.data(), token, mask);

                index_type r;

                if (logger::severity() <= logger::DBG) {
                    logger::log(logger::DBG) << "Derived token: " << hex_string(token) << std::endl;
                }

                bool found = false;
                if (usehdd) {
                    found = edb_.get(token, r);
                } else {
                    if (curArray.count(token) > 0) {
                        found = true;
                        //r = curArray[token];
                    }
                }

                if (found) {
                    if (logger::severity() <= logger::DBG) {
                        logger::log(logger::DBG) << "Found: " << std::hex << r << std::endl;
                    }

                    index_type v = xor_mask(r, mask);

                    post_pool.enqueue(post_callback, v);

                } else {
                    //logger::log(logger::ERROR) << "We were supposed to find a value mapped to key " << hex_string(token);
                    //logger::log(logger::ERROR) << " (" << i << "-th derived key from search token " << hex_string(st) << ")" << std::endl;
                }

            };



            // the rsa job launched with input index,max computes all the RSA tokens of order i + kN up to max
            auto rsa_job = [this, &st, &access_job, &access_pool](const uint8_t index, const size_t max, const uint8_t N) {
                search_token_type local_st = st;
                if (index != 0) {
                    local_st = public_tdp_.eval(local_st, index);
                }

                if (index < max) {
                    // this is a valid search token, we have to derive it and do a lookup
                    access_pool.enqueue(access_job, local_st, index);
                }

                for (size_t i = index + N; i < max; i += N) {
                    local_st = public_tdp_.eval(local_st, N);

                    access_pool.enqueue(access_job, local_st, i);
                }
            };

            std::vector<std::thread> rsa_threads;

            //    unsigned n_threads = std::thread::hardware_concurrency()-access_threads;

            for (uint8_t t = 0; t < rsa_thread_count; t++) {
                rsa_threads.push_back(std::thread(rsa_job, t, req.add_count, rsa_thread_count));
            }

            for (uint8_t t = 0; t < rsa_thread_count; t++) {
                rsa_threads[t].join();
            }

            access_pool.join();
            post_pool.join();
        }

        void SophosServer::search_parallel_light_callback(const SearchRequest& req, std::function<void(index_type) > post_callback, uint8_t thread_count) {
            search_token_type st = req.token;

            if (logger::severity() <= logger::DBG) {
                logger::log(logger::DBG) << "Search token: " << hex_string(req.token) << std::endl;

                logger::log(logger::DBG) << "Derivation key: " << hex_string(req.derivation_key) << std::endl;
            }

            auto derive_access = [&req, this, &post_callback](const search_token_type st, size_t i) {
                update_token_type token;
                std::array<uint8_t, kUpdateTokenSize> mask;
                gen_update_token_masks(req.derivation_key, st.data(), token, mask);

                index_type r;

                if (logger::severity() <= logger::DBG) {
                    logger::log(logger::DBG) << "Derived token: " << hex_string(token) << std::endl;
                }

                bool found = false;
                if (usehdd) {
                    found = edb_.get(token, r);
                } else {
                    if (curArray.count(token) > 0) {
                        found = true;
                        //r = curArray[token];
                    }
                }

                if (found) {
                    if (logger::severity() <= logger::DBG) {
                        logger::log(logger::DBG) << "Found: " << std::hex << r << std::endl;
                    }

                    index_type v = xor_mask(r, mask);

                    post_callback(v);

                } else {
                    //logger::log(logger::ERROR) << "We were supposed to find a value mapped to key " << hex_string(token);
                    //logger::log(logger::ERROR) << " (" << i << "-th derived key from search token " << hex_string(st) << ")" << std::endl;
                }

            };



            // the rsa job launched with input index,max computes all the RSA tokens of order i + kN up to max
            auto job = [this, &st, &derive_access](const uint8_t index, const size_t max, const uint8_t N) {
                search_token_type local_st = st;
                if (index != 0) {
                    local_st = public_tdp_.eval(local_st, index);
                }

                if (index < max) {
                    // this is a valid search token, we have to derive it and do a lookup

                    derive_access(local_st, index);
                }

                for (size_t i = index + N; i < max; i += N) {
                    local_st = public_tdp_.eval(local_st, N);

                    derive_access(local_st, index);
                }
            };

            std::vector<std::thread> rsa_threads;

            //    unsigned n_threads = std::thread::hardware_concurrency()-access_threads;

            for (uint8_t t = 0; t < thread_count; t++) {
                rsa_threads.push_back(std::thread(job, t, req.add_count, thread_count));
            }

            for (uint8_t t = 0; t < thread_count; t++) {
                rsa_threads[t].join();
            }
        }

        void SophosServer::update(const UpdateRequest& req) {
            if (usehdd) {
                edb_.put(req.token, req.index);
            } else {
                curArray.insert(pair<const std::array<uint8_t, 16>, const std::array<uint8_t, 16> >(req.token, req.index));
            }
        }

        std::ostream& SophosServer::print_stats(std::ostream& out) const {
//            out << "Number of tokens: " << edb_.size();
//            out << "; Load: " << edb_.load();
//            out << "; Overflow bucket size: " << edb_.overflow_size() << std::endl;

            return out;
        }

    } // namespace sophos
} // namespace sse
