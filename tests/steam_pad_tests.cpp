// Synthetic host for the Steam Input pad hook (input::initialize_steam).
// It maps the REAL machine code of game.exe RVA 0x22A920..0x22AE9F, read at run
// time from a local analysis copy given on the command line (never stored in
// this repository), into an owned fake image. A fake SteamInput006 object
// implements the MSVC member-function ABI explicitly: GetDigitalActionData and
// GetAnalogActionData receive a hidden RDX output pointer and return it in RAX,
// which the game code dereferences. No game process or Steam client is used.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "dominus/core.hpp"
#include "dominus/input_observer.hpp"
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace dominus;

#define CHECK(condition) do { if (!(condition)) throw std::runtime_error( \
    std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " #condition); } while (false)

constexpr std::size_t ImageSize = 0x5a0000;
constexpr std::uint32_t FunctionRva = 0x22a920, FunctionEnd = 0x22aea0, CookieCheckRva = 0x22f240;
constexpr std::uint32_t ImportRva = 0x26e828, ConstantsRva = 0x52d160, CookieRva = 0x588c48;
constexpr std::uint32_t HandlesRva = 0x59e5a0, EnabledRva = 0x59e668, IdsRva = 0x59e680, ActionsRva = 0x59e6a0;
constexpr std::uint64_t Controller = 0x1122334455667788ull, OtherController = 0x0102030405060708ull;
constexpr std::uint64_t FirstAction = 100;
// Action order of FUN_14022a570: key_up..r3_button, then l_move/r_move.
enum Action : unsigned { Up, Down, Left, Right, A, B, X, Y, Start, Select, L1, L2, L3, R1, R2, R3, Count };

std::uint8_t* image = nullptr;
std::uint32_t pressed = 0;
unsigned digitalCalls = 0, abiFaults = 0;
std::array<void*, 48> vtable{};
void* steamObject[1] = {vtable.data()};
void* steamContext[1] = {steamObject};
std::vector<std::string> logs;

void* context_init(void*) { return steamContext; }
// Explicit MSVC ABI: RCX=this, RDX=out, R8=controller, R9=action, RAX=out.
void* digital_data(void* self, void* out, std::uint64_t controller, std::uint64_t action) {
    ++digitalCalls;
    if (self != steamObject || !out || (controller != Controller && controller != OtherController) ||
        action < FirstAction || action >= FirstAction + Count) { ++abiFaults; return out; }
    auto* bytes = static_cast<std::uint8_t*>(out);
    bytes[0] = (pressed >> (action - FirstAction)) & 1u;
    bytes[1] = 1;
    return out;
}
// InputAnalogActionData_t {EInputSourceMode mode; float x, y; bool active;}.
void* analog_data(void* self, void* out, std::uint64_t, std::uint64_t) {
    if (self != steamObject || !out) { ++abiFaults; return out; }
    std::memset(out, 0, 16);
    return out;
}
int input_type(void* self, std::uint64_t) { if (self != steamObject) ++abiFaults; return 1; }

template<class T> T& at(std::uint32_t rva) { return *reinterpret_cast<T*>(image + rva); }

std::vector<std::uint8_t> read_file(const char* path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot read analysis copy");
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
std::size_t file_offset(const std::vector<std::uint8_t>& file, std::uint32_t rva) {
    const auto pe = *reinterpret_cast<const std::uint32_t*>(&file[0x3c]);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(&file[pe]);
    const auto* section = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section)
        if (rva >= section->VirtualAddress && rva < section->VirtualAddress + section->Misc.VirtualSize)
            return section->PointerToRawData + (rva - section->VirtualAddress);
    throw std::runtime_error("rva outside sections");
}

void build_image(const char* path) {
    const auto file = read_file(path);
    image = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, ImageSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    CHECK(image != nullptr);
    auto& dos = at<IMAGE_DOS_HEADER>(0);
    dos.e_magic = IMAGE_DOS_SIGNATURE; dos.e_lfanew = 0x80;
    auto& nt = at<IMAGE_NT_HEADERS64>(0x80);
    nt.Signature = IMAGE_NT_SIGNATURE; nt.FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
    nt.OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC; nt.OptionalHeader.SizeOfImage = ImageSize;
    std::memcpy(image + FunctionRva, &file[file_offset(file, FunctionRva)], FunctionEnd - FunctionRva);
    std::memcpy(image + ConstantsRva, &file[file_offset(file, ConstantsRva)], 16);
    image[CookieCheckRva] = 0xc3;  // __security_check_cookie stub
    at<void*>(ImportRva) = reinterpret_cast<void*>(&context_init);
    at<std::uint64_t>(CookieRva) = 0x2b992ddfa232ull;
    for (unsigned i = 0; i < 8; ++i) at<std::uint32_t>(IdsRva + 4 * i) = i;
    for (unsigned i = 0; i < 18; ++i) at<std::uint64_t>(ActionsRva + 8 * i) = FirstAction + i;
    vtable[17] = reinterpret_cast<void*>(&digital_data);
    vtable[21] = reinterpret_cast<void*>(&analog_data);
    vtable[37] = reinterpret_cast<void*>(&input_type);
    DWORD old = 0;
    CHECK(VirtualProtect(image + 0x22a000, 0x6000, PAGE_EXECUTE_READ, &old));
    FlushInstructionCache(GetCurrentProcess(), image + 0x22a000, 0x6000);
}
void connect(std::uint64_t handle, bool enabled = true) {
    for (unsigned i = 0; i < 16; ++i) at<std::uint64_t>(HandlesRva + 8 * i) = handle;
    for (unsigned i = 0; i < 8; ++i) at<std::uint8_t>(EnabledRva + i) = enabled ? 1 : 0;
}

struct Pad { int slot; std::uint16_t buttons; std::uint8_t leftTrigger; std::uint32_t packet; };
Pad poll(std::uint32_t buttons, int pad = 0, std::uint16_t previous = 0xcccc) {
    pressed = buttons;
    std::array<std::uint8_t, 16> out{};
    out.fill(0xcc);
    std::memcpy(out.data() + 4, &previous, sizeof(previous));
    using PadFn = int (*)(int, void*);
    const int slot = reinterpret_cast<PadFn>(image + FunctionRva)(pad, out.data());
    Pad result{slot, 0, out[6], 0};
    std::memcpy(&result.buttons, out.data() + 4, 2);
    std::memcpy(&result.packet, out.data(), 4);
    return result;
}
constexpr std::uint32_t bit(Action action) { return 1u << action; }

struct Recorder {
    InputRecognizer recognizer;
    InputPolicy policy{true, true, true, 0};
    std::vector<input::Snapshot> seen;
    std::vector<InputAction> actions;
    void reset(bool enabled) { recognizer.reset(); policy.modEnabled = enabled; seen.clear(); actions.clear(); }
} recorder;
// Mirrors the live adapter: one recognizer per tracked controller identity.
std::uint16_t filter(const input::Snapshot& raw, bool) noexcept {
    try { recorder.seen.push_back(raw); } catch (...) {}
    const InputSnapshot sample{raw.buttons, raw.connected, true, 0};
    const auto result = recorder.recognizer.update(sample, recorder.policy);
    if (result.action != InputAction::None) try { recorder.actions.push_back(result.action); } catch (...) {}
    return result.forwardedButtons;
}
void log(const char* line) noexcept { try { logs.emplace_back(line); } catch (...) {} }

void real_code_maps_steam_actions_to_xinput_bits() {
    connect(Controller);
    CHECK(poll(bit(Select)).buttons == ButtonView);
    CHECK(poll(bit(Y)).buttons == ButtonY);
    CHECK(poll(bit(Select) | bit(Y)).buttons == (ButtonView | ButtonY));
    CHECK(poll(bit(A)).buttons == 0x1000);
    CHECK(poll(bit(Up) | bit(Start)).buttons == 0x0011);
    CHECK(poll(bit(L2)).leftTrigger == 0x1e);
    CHECK(poll(0, 5).slot == 5);
    CHECK(poll(0, 9).slot == -1);
    // packet (+0) is never written by this builder.
    CHECK(poll(bit(Select)).packet == 0xcccccccc);
    CHECK(abiFaults == 0 && digitalCalls > 0);
}
void hook_preserves_abi_and_passes_through_without_filter() {
    CHECK(input::initialize_steam(image, log));
    CHECK(input::initialize_steam(image, log));  // idempotent
    const auto before = digitalCalls;
    for (std::uint32_t buttons : {0u, bit(Select), bit(Y), bit(Select) | bit(Y) | bit(A), bit(L2) | bit(R3)}) {
        CHECK(poll(buttons).slot == 0);
        CHECK(abiFaults == 0);
    }
    CHECK(poll(bit(Select) | bit(Y)).buttons == (ButtonView | ButtonY));
    CHECK(poll(bit(L2)).leftTrigger == 0x1e);
    CHECK(digitalCalls - before == 7 * 16);
}
void view_opens_map_and_never_reaches_the_game() {
    recorder.reset(true);
    input::set_filter(filter);
    CHECK(poll(0).buttons == 0);
    CHECK(poll(bit(Select)).buttons == 0);
    CHECK(poll(bit(Select) | bit(A)).buttons == 0x1000);
    CHECK(poll(bit(Select)).buttons == 0);
    CHECK(recorder.actions.empty());
    CHECK(poll(0).buttons == 0);
    CHECK(recorder.actions.size() == 1 && recorder.actions[0] == InputAction::ToggleMap);
    CHECK(!recorder.seen.empty() && recorder.seen.back().controller == input::SteamController);
    CHECK(recorder.seen.back().connected);
}
void view_y_toggles_mod_and_suppresses_both() {
    recorder.reset(true);
    poll(0);
    CHECK(poll(bit(Select)).buttons == 0);
    CHECK(poll(bit(Select) | bit(Y)).buttons == 0);
    CHECK(poll(bit(Select) | bit(Y) | bit(B)).buttons == 0x2000);
    CHECK(poll(bit(Y)).buttons == 0);  // drained until neutral
    CHECK(poll(0).buttons == 0);
    CHECK(recorder.actions.size() == 1 && recorder.actions[0] == InputAction::ToggleMod);
    CHECK(poll(bit(Y)).buttons == ButtonY);  // Y alone is ordinary input
    poll(0);
}
void disabled_mod_replays_view_once() {
    recorder.reset(false);
    poll(0);
    CHECK(poll(bit(Select)).buttons == 0);
    CHECK(poll(0).buttons == ButtonView);
    CHECK(poll(0).buttons == 0);
    CHECK(recorder.actions.empty());
}
void every_pad_slot_of_one_controller_is_filtered_once() {
    recorder.reset(true);
    for (int pad = 0; pad < 8; ++pad) poll(0, pad);
    for (int pad = 0; pad < 8; ++pad) CHECK(poll(bit(Select), pad).buttons == 0);
    for (int pad = 0; pad < 8; ++pad) CHECK(poll(0, pad).buttons == 0);
    CHECK(recorder.actions.size() == 1 && recorder.actions[0] == InputAction::ToggleMap);
}
void non_steam_paths_and_disconnects_are_untouched() {
    recorder.reset(true);
    poll(0);
    // Disabled pad: builder returns the slot and leaves the caller's word.
    connect(Controller, false);
    recorder.seen.clear();
    CHECK(poll(bit(Select), 0, ButtonView).buttons == ButtonView);
    CHECK(recorder.seen.empty());
    // One empty slot while others still hold the handle is not a disconnect.
    connect(Controller);
    at<std::uint64_t>(HandlesRva + 8 * 5) = 0;
    CHECK(poll(bit(Select), 5, ButtonView).buttons == ButtonView);
    CHECK(recorder.seen.empty());
    // Handle cleared: one disconnected sample, the XInput-derived word stays.
    connect(0);
    CHECK(poll(bit(Select), 0, ButtonView).buttons == ButtonView);
    CHECK(recorder.seen.size() == 1 && !recorder.seen[0].connected);
    CHECK(poll(bit(Select), 0, ButtonView).buttons == ButtonView);
    CHECK(recorder.seen.size() == 1);
    // A new handle becomes the tracked identity.
    connect(OtherController);
    recorder.reset(true);
    poll(0);
    CHECK(poll(bit(Select)).buttons == 0);
    poll(0);
    CHECK(recorder.actions.size() == 1);
    // A second, untracked handle is forwarded unchanged.
    at<std::uint64_t>(HandlesRva + 8 * 3) = Controller;
    CHECK(poll(bit(Select), 3).buttons == ButtonView);
    connect(OtherController);
    CHECK(abiFaults == 0);
}
// What the live filter returns outside verified Portrait gameplay (v0.1.1).
unsigned inactiveCalls = 0;
std::uint16_t inactive_gate(const input::Snapshot& raw, bool) noexcept { ++inactiveCalls; return raw.buttons; }
void inactive_title_gate_forwards_every_action_combination_unchanged() {
    // The tracked identity is still OtherController from the previous test.
    input::set_filter(nullptr);
    poll(0);
    std::vector<Pad> original(1u << Count);
    for (std::uint32_t buttons = 0; buttons < (1u << Count); ++buttons) original[buttons] = poll(buttons);
    inactiveCalls = 0;
    input::set_filter(inactive_gate);
    for (std::uint32_t buttons = 0; buttons < (1u << Count); ++buttons) {
        const auto filtered = poll(buttons);
        CHECK(filtered.slot == original[buttons].slot && filtered.buttons == original[buttons].buttons);
        CHECK(filtered.leftTrigger == original[buttons].leftTrigger && filtered.packet == original[buttons].packet);
    }
    CHECK(inactiveCalls == (1u << Count));
    CHECK(poll(bit(Select)).buttons == ButtonView && poll(bit(Select) | bit(Y)).buttons == (ButtonView | ButtonY));
    input::set_filter(nullptr);
    CHECK(abiFaults == 0);
}
void shutdown_restores_original_code_path() {
    input::shutdown();
    CHECK(poll(bit(Select) | bit(Y)).buttons == (ButtonView | ButtonY));
    CHECK(abiFaults == 0);
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) { std::cerr << "usage: steam_pad_tests.exe <local analysis copy of game.exe>\n"; return 2; }
    try { build_image(argv[1]); } catch (const std::exception& error) { std::cerr << "setup: " << error.what() << '\n'; return 2; }
    struct Test { const char* name; void (*run)(); };
    const Test tests[] = {
        {"real_code_maps_steam_actions_to_xinput_bits", real_code_maps_steam_actions_to_xinput_bits},
        {"hook_preserves_abi_and_passes_through_without_filter", hook_preserves_abi_and_passes_through_without_filter},
        {"view_opens_map_and_never_reaches_the_game", view_opens_map_and_never_reaches_the_game},
        {"view_y_toggles_mod_and_suppresses_both", view_y_toggles_mod_and_suppresses_both},
        {"disabled_mod_replays_view_once", disabled_mod_replays_view_once},
        {"every_pad_slot_of_one_controller_is_filtered_once", every_pad_slot_of_one_controller_is_filtered_once},
        {"non_steam_paths_and_disconnects_are_untouched", non_steam_paths_and_disconnects_are_untouched},
        {"inactive_title_gate_forwards_every_action_combination_unchanged", inactive_title_gate_forwards_every_action_combination_unchanged},
        {"shutdown_restores_original_code_path", shutdown_restores_original_code_path},
    };
    int failures = 0;
    for (const auto& test : tests) {
        try {
            test.run();
            std::cout << "PASS " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << test.name << ": " << error.what() << '\n';
        }
    }
    for (const auto& line : logs) std::cout << "log " << line << '\n';
    std::cout << (sizeof(tests) / sizeof(tests[0])) << " test groups, " << failures << " failures\n";
    return failures ? 1 : 0;
}
