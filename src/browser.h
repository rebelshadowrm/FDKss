#pragma once
#include "fdkss/api.h"
#include <array>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace object_browser {
struct Value {
    fdkss_value_type type = FDKSS_NIL;
    std::int64_t integer = 0;
    double number = 0;
    bool boolean = false;
    std::string text;
};
struct Row { std::string token, type, address, name; };
struct Property { std::string name; Value value; bool writable = false; std::string error; };
struct Watch {
    std::string root, field, error;
    Value value;
    double due = 0;
    std::uint64_t samples = 0;
    bool paused = false;
};
struct View {
    std::vector<Row> rows;
    std::vector<Property> properties;
    std::vector<Watch> watches;
    std::string root, error;
    std::string method_root, method_name, method_error;
    std::vector<Value> method_values;
    std::int64_t next = -1;
    std::uint64_t revision = 0;
};
struct Request {
    enum Kind { page, inspect, write, watch, unwatch, invoke } kind;
    std::string root, field;
    std::int64_t offset = 0;
    double number = 0;
    std::int64_t id = 0; // Assigned by submit; zero is an automatic watch sample.
};
enum class Phase { unknown, pending, succeeded, failed, cancelled, expired };
struct Result { Phase phase = Phase::unknown; std::string error; std::vector<Value> values; };
struct NumericEditor {
    std::string root, field;
    double number = 0;
    bool ready = false;
    void select(std::string_view object, const Property &property) {
        root = object; field = property.name;
        ready = property.error.empty() && property.writable && (property.value.type == FDKSS_NUMBER || property.value.type == FDKSS_INTEGER);
        number = property.value.type == FDKSS_NUMBER ? property.value.number :
            property.value.type == FDKSS_INTEGER ? static_cast<double>(property.value.integer) : 0;
        // The numeric request format cannot faithfully transport larger integers.
        if (property.value.type == FDKSS_INTEGER && (property.value.integer < -9007199254740992LL || property.value.integer > 9007199254740992LL)) ready = false;
        ready = ready && std::isfinite(number);
    }
    bool can_apply(const View &view, std::string_view object, std::string_view name) const {
        if (!ready || !std::isfinite(number) || root != object || field != name || view.root != root) return false;
        return std::any_of(view.properties.begin(), view.properties.end(), [&](const Property &property) {
            return property.name == field && property.error.empty() && property.writable &&
                (property.value.type == FDKSS_NUMBER || property.value.type == FDKSS_INTEGER);
        });
    }
};
class Browser {
    mutable std::mutex mutex_;
    std::deque<Request> requests_;
    View view_;
    std::deque<std::pair<std::int64_t, Result>> results_;
    std::int64_t last_id_ = 0, running_ = 0;
    fdkss_typed_native_fn get_ = nullptr, list_ = nullptr, set_ = nullptr, invoke_ = nullptr;
    bool second_ = false;
    bool closed_ = false;
    void remember(std::int64_t id, Result result) {
        results_.emplace_back(id, std::move(result));
        if (results_.size() > 32) results_.pop_front();
    }
    void cancel() {
        for (const auto &request : requests_) remember(request.id, {Phase::cancelled, "Browser request cancelled before execution"});
        requests_.clear();
    }
    struct Results { std::vector<Value> values; };
    static int FDKSS_CALL collect(void *sink, const fdkss_value *value) {
        auto &output = static_cast<Results *>(sink)->values;
        if (!value || output.size() >= 64 || (value->type == FDKSS_STRING && value->string_size > 4096)) return 1;
        Value copy{value->type, value->integer, value->number, value->boolean != 0, {}};
        if (value->type == FDKSS_STRING) {
            if (!value->string && value->string_size) return 1;
            if (value->string_size) copy.text.assign(value->string, value->string_size);
        }
        output.push_back(std::move(copy)); return 0;
    }
    static fdkss_value string(std::string_view text) {
        fdkss_value result{}; result.type = FDKSS_STRING; result.string = text.data(); result.string_size = text.size(); return result;
    }
    static bool call(fdkss_typed_native_fn callback, const fdkss_value *args, std::size_t size, Results &output, std::string &error) {
        char message[256]{};
        if (!callback) { error = "Engine browser unavailable"; return false; }
        if (callback(nullptr, args, size, collect, &output, message, sizeof(message))) {
            message[sizeof(message) - 1] = 0; error = *message ? message : "Engine request failed"; return false;
        }
        return true;
    }
    bool read(std::string_view root, std::string_view field, Value &value, std::string &error) {
        fdkss_value args[]{string(root), string(field)}; Results result;
        if (!call(get_, args, 2, result, error)) return false;
        if (result.values.size() != 1) { error = "Unexpected engine property result"; return false; }
        value = std::move(result.values[0]); return true;
    }
    void properties(std::string_view root, View &view) {
        Value type, address;
        if (!read(root, "class", type, view.error) || !read(root, "address", address, view.error)) return;
        view.properties.push_back({"class", type}); view.properties.push_back({"address", address});
        if(root.starts_with("script_group:")) {
            for(const char *field : {"group","slot_count","implemented_count"}) {
                Value value;std::string error;
                if(!read(root,field,value,error)) {
                    if(view.error.empty())view.error=std::string(field)+": "+error;
                    view.properties.push_back({field,{},false,std::move(error)});
                }else view.properties.push_back({field,std::move(value),false});
            }
            return;
        }
        const bool character = type.text == ".?AVActor@chara@sora@@";
        const bool actor = !root.empty() && root[0] == '@';
        struct Schema { const char *root, *name; bool writable; };
#define FDKSS_PROPERTY(r, n, offset, tag, storage, editable) {#r, #n, editable},
        static constexpr Schema first[]{
#include "fdkss/sky1_properties.inc"
        }, second[]{
#include "fdkss/sky2_properties.inc"
        };
#undef FDKSS_PROPERTY
        const auto *schema = second_ ? second : first;
        const auto size = second_ ? std::size(second) : std::size(first);
        for (std::size_t index = 0; index < size; ++index) {
            const auto &entry = schema[index];
            const std::string_view category = entry.root;
            if (!(category == root || (actor && category == "actor") ||
                (character && category == "player") || (root == "player" && category == "actor"))) continue;
            bool duplicate = false;
            for (auto &existing : view.properties) if (existing.name == entry.name) {
                existing.writable |= entry.writable; duplicate = true; break;
            }
            if (duplicate) continue;
            Value value; std::string error;
            if (!read(root, entry.name, value, error)) {
                if (view.error.empty()) view.error = std::string(entry.name) + ": " + error;
                view.properties.push_back({entry.name, {}, false, std::move(error)});
                continue;
            }
            view.properties.push_back({entry.name, std::move(value), entry.writable});
        }
    }
public:
    void bind(bool second, fdkss_typed_native_fn get, fdkss_typed_native_fn list, fdkss_typed_native_fn set,
        fdkss_typed_native_fn invoke = nullptr) {
        second_ = second; get_ = get; list_ = list; set_ = set; invoke_ = invoke;
    }
    std::int64_t submit(Request request) {
        if (request.kind == Request::invoke && request.field != "type_name" && request.field != "type_id" &&
            request.field != "type_flags" && request.field != "get_transform") return 0;
        if (request.root.empty() && request.kind != Request::page) return false;
        if (request.root.size() > 255 || request.field.size() > 255 || request.offset < 0 || request.offset > 16384 ||
            ((request.kind == Request::write || request.kind == Request::watch || request.kind == Request::unwatch) && request.field.empty()) ||
            (request.kind == Request::write && !std::isfinite(request.number)) ||
            request.root.find('\0') != std::string::npos || request.field.find('\0') != std::string::npos) return false;
        std::lock_guard lock(mutex_);
        if (closed_ || requests_.size() >= 8 || last_id_ == std::numeric_limits<std::int64_t>::max()) return 0;
        request.id = last_id_ + 1;
        requests_.push_back(std::move(request)); return ++last_id_;
    }
    Result lookup(std::int64_t id) const {
        std::lock_guard lock(mutex_);
        for (const auto &[saved, result] : results_) if (saved == id) return result;
        if (id > 0 && (id == running_ || std::any_of(requests_.begin(), requests_.end(),
            [id](const Request &request) { return request.id == id; }))) return {Phase::pending};
        return {id > 0 && id <= last_id_ ? Phase::expired : Phase::unknown};
    }
    View snapshot() const { std::lock_guard lock(mutex_); return view_; }
    void clear() { std::lock_guard lock(mutex_); cancel(); view_ = {}; }
    void close() { std::lock_guard lock(mutex_); closed_ = true; cancel(); view_ = {}; }
    // Called only on the adapter's verified engine thread, outside all GUI locks.
    bool service(double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count()) {
        if (!std::isfinite(now)) return false;
        Request request;
        {
            std::lock_guard lock(mutex_);
            if (closed_) return false;
            if (requests_.empty()) {
                auto next = view_.watches.end();
                for (auto it = view_.watches.begin(); it != view_.watches.end(); ++it)
                    if (!it->paused && it->due <= now && (next == view_.watches.end() || it->due < next->due)) next = it;
                if (next == view_.watches.end()) return false;
                request = {Request::watch, next->root, next->field};
            } else { request = std::move(requests_.front()); requests_.pop_front(); running_ = request.id; }
        }
        View result = snapshot(); result.error.clear();
        if (request.kind == Request::watch || request.kind == Request::unwatch) {
            auto existing = std::find_if(result.watches.begin(), result.watches.end(), [&](const Watch &watch) {
                return watch.root == request.root && watch.field == request.field;
            });
            if (request.kind == Request::unwatch) {
                if (existing != result.watches.end()) result.watches.erase(existing);
            } else {
                if (existing == result.watches.end() && result.watches.size() == 16) result.error = "Property watch limit is 16";
                else {
                    if (existing == result.watches.end()) {
                        result.watches.push_back({request.root, request.field}); existing = result.watches.end() - 1;
                    }
                    existing->error.clear(); existing->value = {};
                    existing->paused = !read(existing->root, existing->field, existing->value, existing->error);
                    ++existing->samples; existing->due = now + .25;
                    if (request.id && existing->paused) result.error = existing->error;
                }
            }
        } else if (request.kind == Request::invoke) {
            result.method_root=request.root; result.method_name=request.field; result.method_values.clear();
            fdkss_value args[]{string(request.root),string(request.field)}; Results returned;
            if (call(invoke_,args,2,returned,result.error)) {
                const auto count=request.field=="get_transform" ? 16u : 1u;
                bool valid=returned.values.size()==count;
                for (const auto &value : returned.values) {
                    if (request.field=="type_name") valid=valid && value.type==FDKSS_STRING && value.text.size()<128;
                    else if (request.field=="get_transform") valid=valid && value.type==FDKSS_NUMBER && std::isfinite(value.number);
                    else valid=valid && value.type==FDKSS_INTEGER && value.integer>=0 && value.integer<=4294967295LL;
                }
                if (valid) result.method_values=std::move(returned.values);
                else result.error="Unexpected verified actor method result";
            }
            result.method_error=result.error;
        } else if (request.kind == Request::page) {
            fdkss_value args[3]{};
            args[0].type = args[1].type = FDKSS_INTEGER; args[0].integer = request.offset; args[1].integer = 16; args[2] = string(request.root);
            Results list;
            result.rows.clear(); result.next = -1;
            if (call(list_, args, 3, list, result.error)) {
                if (list.values.empty() || list.values.size() > 17 || list.values[0].type != FDKSS_INTEGER ||
                    (list.values[0].integer != -1 && (list.values[0].integer <= request.offset || list.values[0].integer > 16384)))
                    result.error = "Unexpected engine page result";
                else {
                    result.next = list.values[0].integer;
                    for (std::size_t index = 1; index < list.values.size(); ++index) {
                        const auto &token = list.values[index]; Value type, address, name;
                        if (token.type != FDKSS_STRING || !read(token.text, "class", type, result.error) ||
                            !read(token.text, "address", address, result.error) || !read(token.text, "name", name, result.error)) break;
                        result.rows.push_back({token.text, type.text, address.text, name.text});
                    }
                }
            }
        } else {
            result.root = request.root; result.properties.clear();
            if (request.kind == Request::write) {
                Value current;
                if(read(request.root,request.field,current,result.error)) {
                    fdkss_value args[]{string(request.root), string(request.field), {}};
                    if(current.type==FDKSS_INTEGER) {
                        if(std::trunc(request.number)!=request.number || std::abs(request.number)>9007199254740992.0)
                            result.error="Integer property edits require an exactly representable integer";
                        else {args[2].type=FDKSS_INTEGER;args[2].integer=static_cast<std::int64_t>(request.number);}
                    } else if(current.type==FDKSS_NUMBER) {args[2].type=FDKSS_NUMBER;args[2].number=request.number;}
                    else result.error="Property is not an available numeric value";
                    if(result.error.empty()) {Results written;call(set_,args,3,written,result.error);}
                }
            }
            if (result.error.empty()) {
                properties(request.root, result);
                if (request.kind == Request::write && !result.error.empty())
                    result.error = "Write applied; property refresh failed: " + result.error;
            }
        }
        std::lock_guard lock(mutex_);
        if (request.id) {
            remember(request.id, {result.error.empty() ? Phase::succeeded : Phase::failed, result.error,
                request.kind==Request::invoke ? result.method_values : std::vector<Value>{}});
            running_ = 0;
        }
        if (closed_) return false;
        result.revision = view_.revision + 1; view_ = std::move(result); return true;
    }
};
}
