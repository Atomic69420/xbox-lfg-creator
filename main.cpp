#include <iostream>
#include <fstream>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <ctime>
#include <cpprest/http_client.h>
#include <cpprest/json.h>
#include <cpprest/uri.h>
#include <cpprest/asyncrt_utils.h>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/uuid/random_generator.hpp>
#include <pplx/pplxtasks.h>

using namespace web;
using namespace web::http;
using namespace web::http::client;
using namespace utility;
using namespace std;
using namespace chrono;

// Global variables
atomic<size_t> current_token_index{ 0 };
atomic<int> consecutive_401_errors{ 0 };
const int max_consecutive_401_errors = 10;
vector<string> tokens;
int delay;
int threads;
string text;
atomic<int> posts_made{ 0 };
atomic<int> posts_last_interval{ 0 };
atomic<bool> should_continue{ true };
mutex log_mutex;
ofstream log_file;

// Pre-created JSON payloads
json::value base_payload;
json::value base_search_payload;

// Function to log messages with timestamp
void log_message(const string& message, bool console_output = false) {
    auto now = system_clock::now();
    auto now_c = system_clock::to_time_t(now);
    tm local_tm;
    localtime_s(&local_tm, &now_c);

    lock_guard<mutex> lock(log_mutex);
    log_file << put_time(&local_tm, "%Y-%m-%d %H:%M:%S") << " - " << message << endl;
    if (console_output) {
        cout << message << endl;
    }
}

// Function to read tokens from a file
vector<string> read_tokens(const string& file_path) {
    vector<string> tokens;
    ifstream file(file_path);
    string line;
    while (getline(file, line)) {
        tokens.push_back(line);
    }
    return tokens;
}

// Function to create JSON payload
json::value create_payload(const string& session_id) {
    json::value payload = base_payload;
    payload[U("members")][U("me")][U("constants")][U("system")][U("xuid")] = json::value::string(U("2535458394244490"));
    return payload;
}

// Function to create JSON payload for search
json::value create_search_payload(const string& session_id) {
    json::value payload = base_search_payload;
    payload[U("sessionRef")][U("name")] = json::value::string(conversions::to_string_t(session_id));
    return payload;
}

// Worker function to perform operations
void worker(http_client& client, int worker_id) {
    boost::uuids::random_generator uuid_gen;
    int backoff_time = 1000; // Start with 1 second

    while (should_continue.load()) {
        try {
            string session_id = to_string(uuid_gen());

            json::value payload = create_payload(session_id);

            uri_builder builder(U("/serviceconfigs/00000000-0000-0000-0000-000079dbee96/sessiontemplates/global(lfg)/sessions/") + conversions::to_string_t(session_id));
            http_request request(methods::PUT);
            request.set_request_uri(builder.to_uri());
            request.headers().add(U("x-xbl-contract-version"), U("107"));
            request.headers().add(U("authorization"), conversions::to_string_t(tokens[current_token_index]));
            request.headers().add(U("User-Agent"), U("okhttp/3.12.1"));
            request.headers().add(U("X-UserAgent"), U("Android/191121000 SM-A715F.AndroidPhone"));
            request.set_body(payload);

            client.request(request)
                .then([&, worker_id, session_id](http_response response) {
                posts_made++;

                if (response.status_code() == status_codes::Unauthorized || response.status_code() == status_codes::Forbidden) {
                    if (++consecutive_401_errors >= max_consecutive_401_errors) {
                        size_t old_index = current_token_index.load();
                        current_token_index = (old_index + 1) % tokens.size();
                        consecutive_401_errors = 0;
                        log_message("Switched token from index " + to_string(old_index) + " to " + to_string(current_token_index) + " due to consecutive 401 errors");
                    }
                }
                else {
                    consecutive_401_errors = 0;
                }

                json::value payload2 = create_search_payload(session_id);

                http_request search_request(methods::POST);
                search_request.set_request_uri(U("/handles?include=relatedInfo"));
                search_request.set_body(payload2);

                return client.request(search_request);
                    })
                .then([&, session_id](http_response response) {
                pplx::create_task([&, session_id]() {
                    this_thread::sleep_for(milliseconds(delay));

                    uri_builder delete_builder(U("/serviceconfigs/93ac0100-efec-488c-af85-e5850ff4b5bd/sessiontemplates/global(lfg)/sessions/") + conversions::to_string_t(session_id) + U("/members/me"));
                    http_request delete_request(methods::DEL);
                    delete_request.set_request_uri(delete_builder.to_uri());

                    return client.request(delete_request);
                    });
                    })
                .wait();

            backoff_time = 1000; // Reset backoff time on success
        }
        catch (const http_exception& e) {
            log_message("HTTP Exception in worker " + to_string(worker_id) + ": " + e.what());
            this_thread::sleep_for(chrono::milliseconds(backoff_time));
            backoff_time = min(backoff_time * 2, 60000); // Double backoff time, max 1 minute
        }
        catch (const std::exception& e) {
            log_message("Exception in worker " + to_string(worker_id) + ": " + e.what());
            this_thread::sleep_for(chrono::milliseconds(backoff_time));
            backoff_time = min(backoff_time * 2, 60000); // Double backoff time, max 1 minute
        }
        catch (...) {
            log_message("Unknown exception in worker " + to_string(worker_id));
            this_thread::sleep_for(chrono::milliseconds(backoff_time));
            backoff_time = min(backoff_time * 2, 60000); // Double backoff time, max 1 minute
        }
    }
}

// Function to log statistics periodically
void log_stats() {
    auto last_time = chrono::steady_clock::now();
    while (should_continue.load()) {
        this_thread::sleep_for(seconds(3));
        auto current_time = chrono::steady_clock::now();
        auto duration = chrono::duration_cast<chrono::seconds>(current_time - last_time).count();

        int total_posts = posts_made.load();
        int interval_posts = total_posts - posts_last_interval.load();
        double posts_per_second = static_cast<double>(interval_posts) / duration;

        log_message("Total posts: " + to_string(total_posts) +
            ", Posts in last " + to_string(duration) + " seconds: " + to_string(interval_posts) +
            ", Posts per second: " + to_string(posts_per_second), true);

        posts_last_interval.store(total_posts);
        last_time = current_time;
    }
}

int main() {
    try {
        log_file.open("debug_log.txt", ios::out | ios::app);
        log_message("Program started", true);

        tokens = read_tokens("tokens.txt");
        log_message("Loaded " + to_string(tokens.size()) + " tokens", true);

        cout << "What do you want the delay to be? ";
        cin >> delay;
        cout << "How many threads do you want? (I suggest 50 if you want a flooder and 5 if you want it to be normal) ";
        cin >> threads;
        cout << "What do you want as the text? ";
        cin.ignore();
        getline(cin, text);

        log_message("Delay set to " + to_string(delay) + "ms, using " + to_string(threads) + " threads", true);

        // Pre-create base JSON payloads
        base_payload = json::value::object();
        base_payload[U("properties")] = json::value::object();
        base_payload[U("properties")][U("system")] = json::value::object();
        base_payload[U("properties")][U("system")][U("joinRestriction")] = json::value::string(U("followed"));
        base_payload[U("properties")][U("system")][U("readRestriction")] = json::value::string(U("followed"));
        base_payload[U("properties")][U("system")][U("description")] = json::value::object();
        base_payload[U("properties")][U("system")][U("description")][U("locale")] = json::value::string(U("en-US"));
        base_payload[U("properties")][U("system")][U("description")][U("text")] = json::value::string(conversions::to_string_t(text));
        base_payload[U("properties")][U("system")][U("searchHandleVisibility")] = json::value::string(U("xboxlive"));
        base_payload[U("members")] = json::value::object();
        base_payload[U("members")][U("me")] = json::value::object();
        base_payload[U("members")][U("me")][U("constants")] = json::value::object();
        base_payload[U("members")][U("me")][U("constants")][U("system")] = json::value::object();
        base_payload[U("members")][U("me")][U("constants")][U("system")][U("initialize")] = json::value::boolean(true);
        base_payload[U("roleTypes")] = json::value::object();
        base_payload[U("roleTypes")][U("lfg")] = json::value::object();
        base_payload[U("roleTypes")][U("lfg")][U("roles")] = json::value::object();
        base_payload[U("roleTypes")][U("lfg")][U("roles")][U("confirmed")] = json::value::object();
        base_payload[U("roleTypes")][U("lfg")][U("roles")][U("confirmed")][U("target")] = json::value::number(15);

        base_search_payload = json::value::object();
        base_search_payload[U("type")] = json::value::string(U("search"));
        base_search_payload[U("sessionRef")] = json::value::object();
        base_search_payload[U("sessionRef")][U("scid")] = json::value::string(U("93ac0100-efec-488c-af85-e5850ff4b5bd"));
        base_search_payload[U("sessionRef")][U("templateName")] = json::value::string(U("global(lfg)"));
        base_search_payload[U("searchAttributes")] = json::value::object();
        base_search_payload[U("searchAttributes")][U("tags")] = json::value::array();
        base_search_payload[U("searchAttributes")][U("tags")][0] = json::value::string(U("micrequired"));
        base_search_payload[U("searchAttributes")][U("tags")][1] = json::value::string(U("textchatrequired"));
        base_search_payload[U("searchAttributes")][U("achievementIds")] = json::value::array();
        base_search_payload[U("searchAttributes")][U("locale")] = json::value::string(U("en"));

        http_client_config config;
        config.set_timeout(utility::seconds(30));
        http_client client(U("https://sessiondirectory.xboxlive.com"), config);

        vector<thread> workers;
        for (int i = 0; i < threads; ++i) {
            workers.emplace_back(worker, ref(client), i);
        }

        thread stats_thread(log_stats);

        log_message("All threads started", true);

        cout << "Press Enter to stop..." << endl;
        cin.get();

        log_message("Stopping threads...", true);
        should_continue = false;

        for (auto& worker : workers) {
            worker.join();
        }

        stats_thread.join();

        log_message("All threads stopped", true);
        log_message("Program finished", true);
        log_file.close();
    }
    catch (const std::exception& e) {
        log_message("Main thread exception: " + string(e.what()), true);
    }
    catch (...) {
        log_message("Unknown exception in main thread", true);
    }

    return 0;
}