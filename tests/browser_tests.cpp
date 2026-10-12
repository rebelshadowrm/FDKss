#include "browser.h"
#include <cstdio>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <source_location>
namespace {
bool expired = false;
bool refresh_failure = false;
bool unavailable_yaw = false;
bool unavailable_group_count=false;
std::int64_t group_implemented=40;
object_browser::Browser *running_browser = nullptr;
std::int64_t running_id = 0;
unsigned writes = 0;
unsigned reads = 0;
double x = 3;
std::int64_t hp=7;
std::thread::id owner;
void check(bool value,std::source_location at=std::source_location::current()) { if (!value) throw std::runtime_error("object browser boundary check at line "+std::to_string(at.line())); }
int FDKSS_CALL get(void *, const fdkss_value *args, std::size_t count, fdkss_result_fn emit, void *sink, char *error, std::size_t capacity) {
    check(std::this_thread::get_id() == owner && count == 2);
    ++reads;
    const std::string root(args[0].string, args[0].string_size), field(args[1].string, args[1].string_size);
    if (running_browser) check(running_browser->lookup(running_id).phase == object_browser::Phase::pending);
    if (refresh_failure && field == "class") { std::snprintf(error, capacity, "refresh unavailable"); return 1; }
    if (expired && root == "@old") { std::snprintf(error, capacity, "scene actor reference expired"); return 1; }
    if (unavailable_yaw && field == "yaw_radians") { std::snprintf(error, capacity, "yaw requires an upright character"); return 1; }
    if(unavailable_group_count && field=="implemented_count") {std::snprintf(error,capacity,"command group unavailable");return 1;}
    fdkss_value result{};
    std::string text;
    if (field == "class" || field == "address" || field == "name") {
        result.type = FDKSS_STRING; text = field == "class" ? (root.starts_with("script_group:") ? ".?AVCommandSys@script@sora@@" : ".?AVActor@chara@sora@@") : field == "name" ? "Fixture character" : "0x1234";
        result.string = text.data(); result.string_size = text.size();
    } else if(root.starts_with("script_group:")) {result.type=FDKSS_INTEGER;result.integer=field=="group" ? 0 : field=="slot_count" ? 40 : group_implemented;}
    else if(field=="hp"){result.type=FDKSS_INTEGER;result.integer=hp;}
    else { result.type = FDKSS_NUMBER; result.number = field == "position_x" ? x : 1; }
    return emit(sink, &result);
}
int FDKSS_CALL list(void *, const fdkss_value *args, std::size_t count, fdkss_result_fn emit, void *sink, char *, std::size_t) {
    check(std::this_thread::get_id() == owner && count == 3 && args[1].integer == 16);
    fdkss_value result{}; result.type = FDKSS_INTEGER; result.integer = -1;
    if (emit(sink, &result)) return 1;
    std::string token = "@old"; result.type = FDKSS_STRING; result.string = token.data(); result.string_size = token.size();
    return emit(sink, &result);
}
int FDKSS_CALL set(void *, const fdkss_value *args, std::size_t count, fdkss_result_fn, void *, char *error, std::size_t capacity) {
    check(std::this_thread::get_id() == owner && count == 3);
    if (expired) { std::snprintf(error, capacity, "scene actor reference expired"); return 1; }
    if(std::string_view(args[1].string,args[1].string_size)=="hp"){check(args[2].type==FDKSS_INTEGER);++writes;hp=args[2].integer;return 0;}
    if (std::string_view(args[1].string, args[1].string_size) != "position_x") { std::snprintf(error, capacity, "read-only property"); return 1; }
    ++writes; x = args[2].number; return 0;
}
int FDKSS_CALL invoke(void *,const fdkss_value *args,std::size_t count,fdkss_result_fn emit,void *sink,char *error,std::size_t capacity) {
    check(std::this_thread::get_id()==owner && count==2);
    const std::string root(args[0].string,args[0].string_size), method(args[1].string,args[1].string_size);
    if (root=="expired") {std::snprintf(error,capacity,"actor expired");return 1;}
    fdkss_value value{};
    if (method=="type_name") {
        std::string text="Copied actor name";value.type=FDKSS_STRING;value.string=text.data();value.string_size=text.size();return emit(sink,&value);
    }
    if (method=="get_transform") {
        value.type=FDKSS_NUMBER;
        for (unsigned i=0;i<(root=="partial" ? 3u : 16u);++i) {
            value.number=root=="nonfinite" ? std::numeric_limits<double>::infinity() : i+0.5;
            if (emit(sink,&value)) return 1;
        }
        return 0;
    }
    value.type=FDKSS_INTEGER;value.integer=root=="overflow" ? 4294967296LL : 4294967295LL;return emit(sink,&value);
}
void test_method_calls() {
    object_browser::Browser browser;browser.bind(false,get,list,set,invoke);
    check(!browser.submit({object_browser::Request::invoke,"player","set_placement"}));
    std::int64_t id=0;
    std::thread producer([&]{id=browser.submit({object_browser::Request::invoke,"player","type_name"});});producer.join();
    check(browser.lookup(id).phase==object_browser::Phase::pending && browser.snapshot().method_values.empty());
    check(browser.service(1) && browser.lookup(id).phase==object_browser::Phase::succeeded && browser.lookup(id).values[0].text=="Copied actor name");
    const auto saved=id;
    id=browser.submit({object_browser::Request::write,"player","hp",0,123});check(browser.service(1.1));
    check(browser.lookup(id).phase==object_browser::Phase::succeeded && hp==123);
    const auto integer_writes=writes;
    for(const auto number:{1.5,1e20}) {
        id=browser.submit({object_browser::Request::write,"player","hp",0,number});check(browser.service(1.2));
        check(browser.lookup(id).phase==object_browser::Phase::failed && hp==123 && writes==integer_writes);
    }
    const auto inspected=browser.submit({object_browser::Request::inspect,"player"});check(inspected && browser.service(1.5));
    const auto properties=browser.snapshot().properties.size();
    id=browser.submit({object_browser::Request::invoke,"player","get_transform"});check(browser.service(2));
    check(browser.snapshot().properties.size()==properties && browser.snapshot().root=="player");
    check(browser.lookup(id).values.size()==16 && browser.lookup(id).values[15].number==15.5 && browser.lookup(saved).values[0].text=="Copied actor name");
    for (const char *root : {"expired","partial","nonfinite"}) {
        id=browser.submit({object_browser::Request::invoke,root,"get_transform"});check(browser.service(3));
        check(browser.lookup(id).phase==object_browser::Phase::failed && browser.lookup(id).values.empty() && browser.snapshot().method_values.empty());
    }
    id=browser.submit({object_browser::Request::invoke,"player","type_id"});check(browser.service(4));
    check(browser.lookup(id).values.size()==1 && browser.lookup(id).values[0].integer==4294967295LL);
    id=browser.submit({object_browser::Request::invoke,"overflow","type_flags"});check(browser.service(5) && browser.lookup(id).phase==object_browser::Phase::failed);
    id=browser.submit({object_browser::Request::invoke,"player","type_name"});browser.close();
    check(browser.lookup(id).phase==object_browser::Phase::cancelled && !browser.service(6));
}
}
void test_command_groups() {
    object_browser::Browser browser;browser.bind(false,get,list,set);
    const auto id=browser.submit({object_browser::Request::inspect,"script_group:0"});check(id && browser.service(1));
    const auto view=browser.snapshot();check(view.error.empty() && view.properties.size()==5 && view.properties[0].value.text==".?AVCommandSys@script@sora@@");
    object_browser::NumericEditor editor;
    for(const auto &property : view.properties){check(!property.writable);editor.select(view.root,property);check(!editor.ready);}
    const auto denied=browser.submit({object_browser::Request::write,"script_group:0","slot_count",0,1});check(denied && browser.service(1.1) && browser.lookup(denied).phase==object_browser::Phase::failed);
    const auto watch=browser.submit({object_browser::Request::watch,"script_group:0","implemented_count"});check(watch && browser.service(2));
    group_implemented=39;check(browser.service(2.3) && browser.snapshot().watches[0].value.integer==39);
    unavailable_group_count=true;
    const auto failed=browser.submit({object_browser::Request::inspect,"script_group:0"});check(failed && browser.service(3));
    const auto partial=browser.snapshot();check(partial.properties.size()==5 && partial.properties[3].value.integer==40 && !partial.properties[4].error.empty() && browser.lookup(failed).phase==object_browser::Phase::failed);
    unavailable_group_count=false;group_implemented=40;
}
int main() {
    try {
        owner = std::this_thread::get_id();test_method_calls();test_command_groups(); object_browser::Browser browser;
        browser.bind(true, get, list, set);
        std::thread producer([&] {
            check(browser.submit({object_browser::Request::page, "chara"}));
            check(browser.submit({object_browser::Request::inspect, "@old"}));
        }); producer.join();
        check(browser.snapshot().rows.empty());
        check(browser.service()); auto page = browser.snapshot();
        check(page.rows.size() == 1 && page.rows[0].token == "@old" && page.rows[0].type == ".?AVActor@chara@sora@@" && page.next == -1);
        check(page.rows[0].name == "Fixture character");
        check(page.properties.empty() && page.revision == 1); // Exactly one request per service boundary.
        check(browser.service()); auto values = browser.snapshot();
        bool coordinate = false;
        for (const auto &property : values.properties) if (property.name == "position_x") coordinate = property.writable && property.value.number == 3;
        check(coordinate && values.error.empty() && !browser.service());
        object_browser::NumericEditor editor;
        for (const auto &property : values.properties) if (property.name == "position_x") editor.select(values.root, property);
        check(editor.number == 3 && editor.can_apply(values, "@old", "position_x"));
        check(!editor.can_apply(values, "player", "position_x") && !editor.can_apply(values, "@old", "class"));
        auto missing = values; missing.properties.clear();
        check(!editor.can_apply(missing, "@old", "position_x"));
        editor.number = std::numeric_limits<double>::infinity();
        check(!editor.can_apply(values, "@old", "position_x"));
        editor.select(values.root, values.properties.front()); // Read-only string cannot become a numeric write.
        check(!editor.ready && !editor.can_apply(values, values.root, "class"));
        unavailable_yaw = true;
        const auto partial_id = browser.submit({object_browser::Request::inspect, "@old"});
        check(partial_id && browser.service());
        const auto partial = browser.snapshot();
        check(partial.properties.size() == values.properties.size());
        check(browser.lookup(partial_id).phase == object_browser::Phase::failed && partial.error.find("yaw_radians") != std::string::npos);
        bool later_property = false, rejected_yaw = false;
        for (const auto &property : partial.properties) {
            if (property.name == "yaw_radians") { editor.select(partial.root, property); rejected_yaw = !property.error.empty() && !editor.can_apply(partial, partial.root, property.name); }
            if (property.name == "rotation_w") later_property = property.error.empty() && property.value.number == 1;
        }
        check(later_property && rejected_yaw);
        for (const auto &property : partial.properties) if (property.name == "position_x") editor.select(partial.root, property);
        check(editor.can_apply(partial, partial.root, "position_x"));
        // Both schemas retain valid fields after a computed getter fails. A successful
        // write followed by a partial refresh must explicitly report that it applied.
        for (bool second : {false, true}) {
            object_browser::Browser isolated; isolated.bind(second, get, list, set);
            const auto request = isolated.submit({object_browser::Request::write, "player", "position_x", 0, 3});
            check(request && isolated.service());
            const auto result = isolated.lookup(request);
            check(result.phase == object_browser::Phase::failed && result.error.find("Write applied; property refresh failed: yaw_radians") == 0);
            check(isolated.snapshot().properties.size() == partial.properties.size());
        }
        writes = 0;
        unavailable_yaw = false;
        object_browser::Property huge{"large", {}, true}; huge.value.type = FDKSS_INTEGER;
        huge.value.integer = 9007199254740993LL; editor.select(values.root, huge); check(!editor.ready);
        expired = true;
        check(browser.submit({object_browser::Request::write, "@old", "position_x", 0, 9}));
        check(browser.service() && writes == 0 && x == 3 && browser.snapshot().error == "scene actor reference expired");
        check(browser.snapshot().properties.empty());
        expired = false;
        check(browser.submit({object_browser::Request::write, "player", "class", 0, 9}));
        check(browser.service() && writes == 0 && browser.snapshot().error == "Property is not an available numeric value");
        check(browser.submit({object_browser::Request::write, "player", "position_x", 0, 9}));
        check(browser.service() && writes == 1 && x == 9 && browser.snapshot().error.empty());
        check(!browser.submit({object_browser::Request::write, "player", "position_x", 0, std::numeric_limits<double>::infinity()}));
        for (int index = 0; index < 8; ++index) check(browser.submit({object_browser::Request::inspect, "player"}));
        check(!browser.submit({object_browser::Request::inspect, "player"}));
        browser.clear(); check(!browser.service() && browser.snapshot().properties.empty());
        check(browser.submit({object_browser::Request::watch, "@old", "position_x"}));
        check(browser.service(0));
        auto watched = browser.snapshot().watches;
        check(watched.size() == 1 && watched[0].samples == 1 && watched[0].value.number == 9);
        const auto baseline = reads;
        check(!browser.service(.249) && reads == baseline);
        x = 12;
        check(browser.service(.25));
        watched = browser.snapshot().watches;
        check(watched[0].samples == 2 && watched[0].value.number == 12 && reads == baseline + 1);
        expired = true;
        check(browser.service(10)); // Coalesce missed intervals to one read.
        watched = browser.snapshot().watches;
        check(watched[0].samples == 3 && watched[0].paused && watched[0].error == "scene actor reference expired");
        check(watched[0].value.type == FDKSS_NIL && !browser.service(20));
        expired = false;
        check(browser.submit({object_browser::Request::watch, "@old", "position_x"}));
        check(browser.service(20) && browser.snapshot().watches.size() == 1 && !browser.snapshot().watches[0].paused);
        check(browser.submit({object_browser::Request::unwatch, "@old", "position_x"}));
        check(browser.service(20) && browser.snapshot().watches.empty() && !browser.service(21));
        for (int index = 0; index < 16; ++index) {
            check(browser.submit({object_browser::Request::watch, "player", "field" + std::to_string(index)}));
            check(browser.service(0));
        }
        check(browser.submit({object_browser::Request::watch, "player", "overflow"}));
        check(browser.service(0) && browser.snapshot().watches.size() == 16 && browser.snapshot().error == "Property watch limit is 16");
        const auto before = reads;
        for (int index = 0; index < 16; ++index) check(browser.service(1));
        check(reads == before + 16 && !browser.service(1)); // One read per boundary; all watches get a turn.
        for (const auto &watch : browser.snapshot().watches) check(watch.samples == 2);
        check(browser.submit({object_browser::Request::inspect, "framework"}));
        check(browser.service(2) && browser.snapshot().root == "framework"); // Explicit jobs take priority over due watches.
        check(!browser.submit({object_browser::Request::watch, "player", ""}));
        check(!browser.submit({object_browser::Request::watch, std::string("player\0other", 12), "class"}));
        browser.close(); check(!browser.submit({object_browser::Request::write, "player", "position_x", 0, 12}) && writes == 1);
        check(!browser.service(100));
        object_browser::Browser tracked;
        tracked.bind(true, get, list, set);
        check(tracked.lookup(0).phase == object_browser::Phase::unknown);
        const auto failure_id = tracked.submit({object_browser::Request::write, "player", "class", 0, 9});
        const auto success_id = tracked.submit({object_browser::Request::inspect, "player"});
        check(failure_id > 0 && success_id > failure_id);
        check(tracked.lookup(failure_id).phase == object_browser::Phase::pending);
        check(tracked.service(0) && tracked.lookup(failure_id).phase == object_browser::Phase::failed);
        check(tracked.lookup(failure_id).error == "Property is not an available numeric value");
        check(tracked.lookup(success_id).phase == object_browser::Phase::pending);
        check(tracked.service(0) && tracked.lookup(success_id).phase == object_browser::Phase::succeeded);
        check(tracked.snapshot().error.empty() && tracked.lookup(failure_id).error == "Property is not an available numeric value");
        expired = true;
        const auto watch_id = tracked.submit({object_browser::Request::watch, "@old", "position_x"});
        check(tracked.service(0) && tracked.lookup(watch_id).phase == object_browser::Phase::failed);
        expired = false;
        const auto retry_id = tracked.submit({object_browser::Request::watch, "@old", "position_x"});
        check(tracked.service(0) && tracked.lookup(retry_id).phase == object_browser::Phase::succeeded);
        check(tracked.service(1) && tracked.lookup(failure_id).error == "Property is not an available numeric value"); // Automatic samples do not replace results.
        const auto refresh_id = tracked.submit({object_browser::Request::write, "player", "position_x", 0, 17});
        running_browser = &tracked; running_id = refresh_id; refresh_failure = true;
        check(tracked.service(1) && x == 17 && writes == 2);
        check(tracked.lookup(refresh_id).phase == object_browser::Phase::failed &&
            tracked.lookup(refresh_id).error == "Write applied; property refresh failed: refresh unavailable");
        running_browser = nullptr; refresh_failure = false;
        const auto cancelled_id = tracked.submit({object_browser::Request::inspect, "player"});
        tracked.clear();
        check(tracked.lookup(cancelled_id).phase == object_browser::Phase::cancelled && !tracked.service(2));
        for (int index = 0; index < 33; ++index) {
            const auto id = tracked.submit({object_browser::Request::inspect, "player"});
            check(id > cancelled_id && tracked.service(2));
        }
        check(tracked.lookup(failure_id).phase == object_browser::Phase::expired);
        check(tracked.lookup(1000000).phase == object_browser::Phase::unknown);
        const auto stopped_id = tracked.submit({object_browser::Request::write, "player", "position_x", 0, 100});
        tracked.close();
        check(tracked.lookup(stopped_id).phase == object_browser::Phase::cancelled && !tracked.service(3) && writes == 2 && x == 17);
        std::cout << "Browser owner boundary, copied values, stale/write guards, watch scheduling and limits passed\n";
        return 0;
    } catch (const std::exception &failure) { std::cerr << failure.what() << '\n'; return 1; }
}
