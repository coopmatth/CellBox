// frontend_library.cpp - see frontend_library.h. No firmware, keys or
// game content is read beyond PARAM.SFO metadata + file sizes.
#include "frontend_library.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// NOTE: this TU uses the FromApp file APIs (declared in fileapifromapp.h
// behind WINAPI_PARTITION_APP); build-frontend.ps1 compiles it with
// /DWINAPI_FAMILY=WINAPI_FAMILY_APP in both the desktop pre-test and the
// UWP build so the identical code runs in both.
#include <windows.h>
#include <fileapifromapp.h> // FromApp file APIs (GetFileAttributesExFromApp etc.) are not declared by fileapi.h under the desktop family
#include <fileapifromapp.h>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace frontend
{
namespace
{

std::wstring wide_from_utf8(const std::string& s)
{
    std::wstring out;
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return out;
    out.resize(static_cast<size_t>(n));
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
    if (!out.empty() && out.back() == L'\0') out.pop_back();
    return out;
}

std::string utf8_from_wide(const std::wstring& s)
{
    std::string out;
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return out;
    out.resize(static_cast<size_t>(n));
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, out.data(), n, nullptr, nullptr);
    if (!out.empty() && out.back() == '\0') out.pop_back();
    return out;
}

std::string slash(const std::string& p)
{
    std::string s = p;
    std::replace(s.begin(), s.end(), '\\', '/');
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}

bool path_attrs(const std::wstring& wpath, WIN32_FILE_ATTRIBUTE_DATA& fad)
{
    return GetFileAttributesExFromAppW(wpath.c_str(), GetFileExInfoStandard, &fad) != FALSE;
}

bool is_dir_utf8(const std::string& p)
{
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    return path_attrs(wide_from_utf8(p), fad) && (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool is_file_utf8(const std::string& p, std::uint64_t* sizeOut = nullptr)
{
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!path_attrs(wide_from_utf8(p), fad)) return false;
    if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return false;
    if (sizeOut)
        *sizeOut = (static_cast<std::uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    return true;
}

struct DirItem
{
    std::string name; // UTF-8, single component
    bool is_dir = false;
    std::uint64_t size = 0;
};

// One-level listing via FindFirstFileExFromApp (app-partition API).
// Returns false + win32 error when the directory cannot be opened.
bool list_dir(const std::string& dirUtf8, std::vector<DirItem>& out, unsigned long& errOut)
{
    out.clear();
    errOut = 0;
    std::wstring pattern = wide_from_utf8(slash(dirUtf8));
    for (wchar_t& c : pattern) if (c == L'/') c = L'\\';
    if (!pattern.empty() && pattern.back() != L'\\') pattern += L'\\';
    pattern += L'*';
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileExFromAppW(pattern.c_str(), FindExInfoBasic, &fd,
                                      FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) { errOut = GetLastError(); return false; }
    do
    {
        if (fd.cFileName[0] == L'.' &&
            (fd.cFileName[1] == L'\0' || (fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0')))
            continue;
        DirItem it;
        it.name = utf8_from_wide(fd.cFileName);
        it.is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        it.size = (static_cast<std::uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        out.push_back(it);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return true;
}

// Read a whole small file (PARAM.SFO). CreateFile2: console-proven.
bool read_small_file(const std::string& pathUtf8, std::vector<std::uint8_t>& out,
                     unsigned long& errOut)
{
    out.clear();
    errOut = 0;
    HANDLE h = CreateFile2(wide_from_utf8(pathUtf8).c_str(), GENERIC_READ,
                           FILE_SHARE_READ, OPEN_EXISTING, nullptr);
    if (h == INVALID_HANDLE_VALUE) { errOut = GetLastError(); return false; }
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 0 || sz.QuadPart > (4 << 20))
    {
        errOut = GetLastError();
        CloseHandle(h);
        return false;
    }
    out.resize(static_cast<size_t>(sz.QuadPart));
    DWORD got = 0;
    BOOL ok = out.empty() ||
              ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &got, nullptr);
    if (!ok || got != out.size()) errOut = GetLastError();
    CloseHandle(h);
    return ok && got == out.size();
}

// Recursive size with hard caps so a huge tree cannot stall the menu.
std::uint64_t folder_size(const std::string& dirUtf8, int depth, unsigned& budget)
{
    if (depth > 8 || budget == 0) return 0;
    std::vector<DirItem> items;
    unsigned long err = 0;
    if (!list_dir(dirUtf8, items, err)) return 0;
    std::uint64_t total = 0;
    for (const DirItem& it : items)
    {
        if (budget == 0) break;
        --budget;
        if (it.is_dir) total += folder_size(dirUtf8 + "/" + it.name, depth + 1, budget);
        else total += it.size;
    }
    return total;
}

bool ends_with_iso(const std::string& name)
{
    if (name.size() < 4) return false;
    std::string ext = name.substr(name.size() - 4);
    for (char& c : ext) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return ext == ".iso";
}

std::string stem_of(const std::string& name)
{
    size_t dot = name.find_last_of('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

void fill_sfo(GameEntry& e, const std::string& sfoPath, const LogFn& log)
{
    std::vector<std::uint8_t> bytes;
    unsigned long err = 0;
    if (!read_small_file(sfoPath, bytes, err))
    {
        if (log) log("FRONTEND-LIB-SFO read failed path=" + sfoPath +
                     " err=" + std::to_string(err));
        return;
    }
    std::string title, serial, category;
    if (parse_param_sfo(bytes.data(), bytes.size(), title, serial, category))
    {
        e.sfo_ok = true;
        if (!title.empty()) e.title = title;
        if (!serial.empty()) e.serial = serial;
    }
    else if (log)
    {
        log("FRONTEND-LIB-SFO parse failed (not PSF 1.01) path=" + sfoPath);
    }
}

// Classify one candidate folder. Returns true when it looks like a game:
//  shape A: <cand>/PS3_GAME/PARAM.SFO (+USRDIR/EBOOT.BIN) -> boot <cand>
//  shape B: <cand>/PARAM.SFO + <cand>/USRDIR/EBOOT.BIN (PS3_GAME itself)
//           -> boot the EBOOT.BIN path directly
//  shape C: <cand>/USRDIR/EBOOT.BIN only -> boot EBOOT, sfo_ok=false
bool classify_folder(const std::string& candUtf8, const std::string& displayName,
                     GameSource source, GameEntry& e, const LogFn& log)
{
    const std::string cand = slash(candUtf8);
    e = GameEntry{};
    e.title = displayName;
    e.serial = "-";
    e.source = source;
    e.kind = GameKind::Folder;

    const std::string sfoA = cand + "/PS3_GAME/PARAM.SFO";
    const std::string ebootA = cand + "/PS3_GAME/USRDIR/EBOOT.BIN";
    const std::string sfoB = cand + "/PARAM.SFO";
    const std::string ebootB = cand + "/USRDIR/EBOOT.BIN";

    if (is_file_utf8(sfoA))
    {
        e.path = cand; // disc root: BootGame mounts dev_bdvd from PS3_GAME
        fill_sfo(e, sfoA, log);
        if (log) log("FRONTEND-LIB-SHAPE disc-root eboot_present=" +
                     std::string(is_file_utf8(ebootA) ? "1" : "0") + " path=" + cand);
        return true;
    }
    if (is_file_utf8(ebootB))
    {
        e.path = ebootB;
        if (is_file_utf8(sfoB)) fill_sfo(e, sfoB, log);
        if (log) log("FRONTEND-LIB-SHAPE ps3_game-direct path=" + ebootB);
        return true;
    }
    if (is_file_utf8(ebootA))
    {
        e.path = cand;
        if (log) log("FRONTEND-LIB-SHAPE disc-root-no-sfo path=" + cand);
        return true;
    }
    return false;
}

void finish_entry(GameEntry& e, const std::string& lsSlash, bool computeSize)
{
    if (computeSize && e.kind == GameKind::Folder)
    {
        // Size the game folder, not the EBOOT file, for shape B.
        std::string dir = e.path;
        const std::string suffix = "/USRDIR/EBOOT.BIN";
        if (dir.size() > suffix.size() &&
            dir.compare(dir.size() - suffix.size(), suffix.size(), suffix) == 0)
            dir.resize(dir.size() - suffix.size());
        unsigned budget = 20000;
        e.size_bytes = folder_size(dir, 0, budget);
    }
    if (e.sfo_ok && e.serial != "-" && e.serial != "ISO")
        e.has_cache = is_dir_utf8(lsSlash + "/cache/" + e.serial);
}

void log_entries(const std::vector<GameEntry>& v, size_t firstIdx, const LogFn& log)
{
    if (!log) return;
    for (size_t i = firstIdx; i < v.size(); ++i)
    {
        const GameEntry& e = v[i];
        log("FRONTEND-LIB-ENTRY idx=" + std::to_string(i) +
            " source=" + game_source_name(e.source) +
            " kind=" + game_kind_name(e.kind) +
            " sfo=" + std::string(e.sfo_ok ? "1" : "0") +
            " cache=" + std::string(e.has_cache ? "1" : "0") +
            " size=" + std::to_string(e.size_bytes) +
            " serial=" + e.serial +
            " title=" + e.title +
            " path=" + e.path);
    }
}

} // namespace

const char* game_source_name(GameSource s)
{
    switch (s)
    {
    case GameSource::LocalState: return "LocalState";
    case GameSource::LocalIso: return "LocalIso";
    case GameSource::Usb: return "USB";
    }
    return "?";
}

const char* game_kind_name(GameKind k)
{
    return k == GameKind::Iso ? "iso" : "folder";
}

bool parse_param_sfo(const std::uint8_t* data, std::size_t size,
                     std::string& title, std::string& serial,
                     std::string& category)
{
    title.clear(); serial.clear(); category.clear();
    if (!data || size < 20) return false;
    auto rd32 = [&](std::size_t off) -> std::uint32_t {
        return static_cast<std::uint32_t>(data[off]) |
               (static_cast<std::uint32_t>(data[off + 1]) << 8) |
               (static_cast<std::uint32_t>(data[off + 2]) << 16) |
               (static_cast<std::uint32_t>(data[off + 3]) << 24);
    };
    auto rd16 = [&](std::size_t off) -> std::uint16_t {
        return static_cast<std::uint16_t>(data[off] |
               (static_cast<std::uint16_t>(data[off + 1]) << 8));
    };
    // header_t: magic "\0PSF" (PSF.cpp: header.magic == "\0PSF"_u32),
    // version 0x101, off_key_table, off_data_table, entries_num.
    if (!(data[0] == 0x00 && data[1] == 'P' && data[2] == 'S' && data[3] == 'F'))
        return false;
    if (rd32(4) != 0x101u) return false;
    const std::uint32_t offKeys = rd32(8);
    const std::uint32_t offData = rd32(12);
    const std::uint32_t num = rd32(16);
    if (offKeys < 20 || offKeys > offData || offData > size) return false;
    if (num > 4096) return false;
    if (20ull + static_cast<unsigned long long>(num) * 16ull > offKeys &&
        20ull + static_cast<unsigned long long>(num) * 16ull > size)
        return false;
    if (20ull + static_cast<unsigned long long>(num) * 16ull > size) return false;

    const std::size_t keyTableLen = offData - offKeys;
    for (std::uint32_t i = 0; i < num; ++i)
    {
        const std::size_t defOff = 20 + static_cast<std::size_t>(i) * 16;
        const std::uint16_t keyOff = rd16(defOff + 0);
        const std::uint16_t fmt = rd16(defOff + 2); // psf::format (LE u16)
        const std::uint32_t len = rd32(defOff + 4);
        const std::uint32_t maxLen = rd32(defOff + 8);
        const std::uint32_t dataOff = rd32(defOff + 12);
        if (keyOff >= keyTableLen) return false;
        if (len > maxLen) return false;
        if (static_cast<unsigned long long>(offData) + dataOff + len > size)
            return false;
        const char* keyPtr = reinterpret_cast<const char*>(data + offKeys + keyOff);
        std::string key(keyPtr, strnlen(keyPtr, keyTableLen - keyOff));
        if (fmt != 0x0204 && fmt != 0x0004) continue; // string/array only
        std::string value(reinterpret_cast<const char*>(data + offData + dataOff), len);
        if (fmt == 0x0204)
        {
            if (size_t nts = value.find_first_of('\0'); nts != std::string::npos)
                value.resize(nts);
        }
        if (key == "TITLE") title = value;
        else if (key == "TITLE_ID") serial = value;
        else if (key == "CATEGORY") category = value;
    }
    return true;
}

namespace
{

std::string lower_of(std::string s)
{
    for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

bool has_dot_component(const std::string& p)
{
    size_t i = 0;
    while (i <= p.size())
    {
        size_t j = p.find('/', i);
        if (j == std::string::npos) j = p.size();
        const std::string comp = p.substr(i, j - i);
        if (comp == ".." || comp == ".") return true;
        if (j == p.size()) break;
        i = j + 1;
    }
    return false;
}

bool delete_file_utf8(const std::string& p, unsigned long& errOut)
{
    errOut = 0;
    if (DeleteFileFromAppW(wide_from_utf8(p).c_str())) return true;
    errOut = GetLastError();
    return false;
}

bool remove_dir_utf8(const std::string& p, unsigned long& errOut)
{
    errOut = 0;
    if (RemoveDirectoryFromAppW(wide_from_utf8(p).c_str())) return true;
    errOut = GetLastError();
    return false;
}

struct DeleteWalk
{
    std::uint32_t files = 0;
    std::uint64_t bytes = 0;
    std::uint32_t fail_count = 0;
    std::string first_fail_path;
    unsigned long first_fail_err = 0;
    void fail(const std::string& p, unsigned long err)
    {
        if (fail_count == 0) { first_fail_path = p; first_fail_err = err; }
        ++fail_count;
    }
};

// Post-order recursive delete under dir (files first, then subdirs).
// Best-effort: a failure is recorded and the walk continues; the
// caller judges by fail_count. Depth-capped like folder_size.
void delete_tree_rec(const std::string& dir, int depth, DeleteWalk& w)
{
    if (depth > 12) { w.fail(dir, 0); return; }
    std::vector<DirItem> items;
    unsigned long err = 0;
    if (!list_dir(dir, items, err)) { w.fail(dir, err); return; }
    for (const DirItem& it : items)
    {
        const std::string child = dir + "/" + it.name;
        if (it.is_dir)
        {
            delete_tree_rec(child, depth + 1, w);
            unsigned long e2 = 0;
            if (!remove_dir_utf8(child, e2) && is_dir_utf8(child)) w.fail(child, e2);
        }
        else
        {
            unsigned long e2 = 0;
            if (delete_file_utf8(child, e2)) { ++w.files; w.bytes += it.size; }
            else if (is_file_utf8(child)) w.fail(child, e2);
        }
    }
}

} // namespace

BootTarget resolve_boot_target(const GameEntry& e)
{
    BootTarget t;
    t.path = e.path;
    if (e.kind == GameKind::Iso) { t.form = "iso-file"; return t; }
    const std::string p = slash(e.path);
    const std::string ebootSuffix = "/USRDIR/EBOOT.BIN";
    if (p.size() > ebootSuffix.size() &&
        lower_of(p.substr(p.size() - ebootSuffix.size())) == lower_of(ebootSuffix))
    {
        t.form = "eboot-path";
        return t;
    }
    const size_t pos = p.find_last_of('/');
    const std::string last = pos == std::string::npos ? p : p.substr(pos + 1);
    if (last == "PS3_GAME") { t.form = "ps3_game-dir"; return t; }
    t.form = "disc-root";
    return t;
}

DeleteResult delete_entry(const GameEntry& e, const std::string& localStateUtf8,
                          const LogFn& log)
{
    DeleteResult r;
    const std::string ls = slash(localStateUtf8);
    if (log)
        log("FRONTEND-DELETE requested source=" + std::string(game_source_name(e.source)) +
            " kind=" + game_kind_name(e.kind) + " serial=" + e.serial +
            " title=" + e.title + " path=" + e.path);
    if (e.source == GameSource::Usb)
    {
        r.refused = true;
        if (log)
            log("FRONTEND-DELETE refused reason=usb-read-only-source path=" + e.path +
                " (USB games can't be deleted from here)");
        return r;
    }
    if (e.kind == GameKind::Folder)
    {
        const std::string p = slash(e.path);
        std::string root = p;
        const std::string ebootSuffix = "/USRDIR/EBOOT.BIN";
        if (p.size() > ebootSuffix.size() &&
            lower_of(p.substr(p.size() - ebootSuffix.size())) == lower_of(ebootSuffix))
            root = p.substr(0, p.size() - ebootSuffix.size());
        const std::string gamesRoot = ls + "/games";
        const bool under = root.size() > gamesRoot.size() + 1 &&
                           root.compare(0, gamesRoot.size(), gamesRoot) == 0 &&
                           root[gamesRoot.size()] == '/';
        if (!under || has_dot_component(root))
        {
            r.refused = true;
            if (log)
                log("FRONTEND-DELETE refused reason=path-not-strictly-under-games root=" + root +
                    " games=" + gamesRoot + " path=" + e.path);
            return r;
        }
        DeleteWalk w;
        delete_tree_rec(root, 0, w);
        unsigned long err = 0;
        bool rootGone = !is_dir_utf8(root);
        if (!rootGone)
        {
            if (remove_dir_utf8(root, err)) rootGone = true;
            else if (!is_dir_utf8(root)) rootGone = true;
            else w.fail(root, err);
        }
        r.files_removed = w.files;
        r.bytes_removed = w.bytes;
        r.ok = (w.fail_count == 0) && rootGone;
        r.fail_path = w.first_fail_path;
        r.fail_err = w.first_fail_err;
        if (log)
        {
            if (w.fail_count)
                log("FRONTEND-DELETE-FAIL first_path=" + w.first_fail_path +
                    " err=" + std::to_string(w.first_fail_err) +
                    " failures=" + std::to_string(w.fail_count));
            log("FRONTEND-DELETE-RESULT target=" + root +
                " files=" + std::to_string(r.files_removed) +
                " bytes=" + std::to_string(r.bytes_removed) +
                " ok=" + (r.ok ? "1" : "0"));
            log("FRONTEND-DELETE done ok=" + std::string(r.ok ? "1" : "0") +
                " files=" + std::to_string(r.files_removed) +
                " bytes=" + std::to_string(r.bytes_removed) +
                " (PPU cache under LocalState/cache deliberately kept)");
        }
        return r;
    }
    // GameKind::Iso: a single file in games/ (LocalState) or the
    // legacy LocalState-root file (LocalIso) with its staging parts.
    const std::string p = slash(e.path);
    const std::string gamesRoot = ls + "/games";
    const size_t pos = p.find_last_of('/');
    const std::string parent = pos == std::string::npos ? std::string() : p.substr(0, pos);
    const std::string name = pos == std::string::npos ? p : p.substr(pos + 1);
    const bool parentOk = (e.source == GameSource::LocalIso && parent == ls) ||
                          (e.source == GameSource::LocalState && parent == gamesRoot);
    if (!parentOk || has_dot_component(p))
    {
        r.refused = true;
        if (log)
            log("FRONTEND-DELETE refused reason=iso-path-unexpected parent=" + parent +
                " path=" + e.path);
        return r;
    }
    bool allOk = true;
    auto delete_one = [&](const std::string& target, const char* tag) {
        std::uint64_t size = 0;
        if (!is_file_utf8(target, &size))
        {
            if (log)
                log(std::string("FRONTEND-DELETE-RESULT target=") + target +
                    " tag=" + tag + " files=0 bytes=0 ok=1 (already absent)");
            return;
        }
        unsigned long err = 0;
        if (delete_file_utf8(target, err))
        {
            ++r.files_removed;
            r.bytes_removed += size;
            if (log)
                log(std::string("FRONTEND-DELETE-RESULT target=") + target +
                    " tag=" + tag + " files=1 bytes=" + std::to_string(size) + " ok=1");
        }
        else
        {
            allOk = false;
            if (r.fail_path.empty()) { r.fail_path = target; r.fail_err = err; }
            if (log)
                log(std::string("FRONTEND-DELETE-FAIL path=") + target +
                    " err=" + std::to_string(err) + " tag=" + tag);
        }
    };
    delete_one(p, "iso");
    if (e.source == GameSource::LocalIso && name == kStagingIsoName)
    {
        // The staging block would reassemble the ISO from these at
        // the next launch; deleting the ISO alone resurrects it.
        for (int i = 0; i < kStagingPartCount; ++i)
            delete_one(ls + "/" + kStagingPartBase + ".part" + std::to_string(i), "iso-part");
    }
    r.ok = allOk;
    if (log)
        log("FRONTEND-DELETE done ok=" + std::string(r.ok ? "1" : "0") +
            " files=" + std::to_string(r.files_removed) +
            " bytes=" + std::to_string(r.bytes_removed) +
            " (PPU cache under LocalState/cache deliberately kept)");
    return r;
}

std::vector<GameEntry> scan_localstate(const std::string& localStateUtf8, const LogFn& log)
{
    std::vector<GameEntry> out;
    const std::string ls = slash(localStateUtf8);
    const std::string gamesDir = ls + "/games";
    std::vector<DirItem> items;
    unsigned long err = 0;
    const bool listed = list_dir(gamesDir, items, err);
    if (log)
        log("FRONTEND-LIB-SCAN source=LocalState dir=" + gamesDir +
            " listed=" + std::string(listed ? "1" : "0") +
            " items=" + std::to_string(items.size()) +
            " err=" + std::to_string(err) +
            " (err=2/3 = games dir absent: empty library, not a failure)");
    if (listed)
    {
        for (const DirItem& it : items)
        {
            if (it.is_dir)
            {
                GameEntry e;
                if (classify_folder(gamesDir + "/" + it.name, it.name,
                                    GameSource::LocalState, e, log))
                {
                    finish_entry(e, ls, true);
                    out.push_back(e);
                }
            }
            else if (ends_with_iso(it.name))
            {
                GameEntry e;
                e.title = stem_of(it.name);
                e.serial = "ISO";
                e.path = gamesDir + "/" + it.name;
                e.source = GameSource::LocalState;
                e.kind = GameKind::Iso;
                e.size_bytes = it.size;
                out.push_back(e);
            }
        }
    }
    // Legacy route: *.iso directly in LocalState root (1.0.x uploads).
    std::vector<DirItem> rootItems;
    unsigned long err2 = 0;
    if (list_dir(ls, rootItems, err2))
    {
        for (const DirItem& it : rootItems)
        {
            if (!it.is_dir && ends_with_iso(it.name))
            {
                GameEntry e;
                e.title = stem_of(it.name);
                e.serial = "ISO";
                e.path = ls + "/" + it.name;
                e.source = GameSource::LocalIso;
                e.kind = GameKind::Iso;
                e.size_bytes = it.size;
                out.push_back(e);
                if (log)
                    log("FRONTEND-LIB-LEGACY-ISO LocalState-root iso listed name=" + it.name);
            }
        }
    }
    std::sort(out.begin(), out.end(),
              [](const GameEntry& a, const GameEntry& b) { return a.title < b.title; });
    log_entries(out, 0, log);
    if (log)
        log("FRONTEND-LIB-SCAN-DONE source=LocalState entries=" + std::to_string(out.size()));
    return out;
}

std::vector<GameEntry> scan_usb_root(const std::string& rootUtf8,
                                     const std::string& localStateUtf8,
                                     const LogFn& log)
{
    std::vector<GameEntry> out;
    const std::string root = slash(rootUtf8);
    const std::string ls = slash(localStateUtf8);
    // The root itself may be a disc root or a bare PS3_GAME.
    {
        GameEntry e;
        size_t namePos = root.find_last_of('/');
        std::string name = namePos == std::string::npos ? root : root.substr(namePos + 1);
        if (classify_folder(root, name.empty() ? "USB root" : name, GameSource::Usb, e, log))
        {
            finish_entry(e, ls, true);
            out.push_back(e);
        }
    }
    std::vector<DirItem> items;
    unsigned long err = 0;
    const bool listed = list_dir(root, items, err);
    if (log)
        log("FRONTEND-USB-SCAN root=" + root +
            " listed=" + std::string(listed ? "1" : "0") +
            " items=" + std::to_string(items.size()) +
            " err=" + std::to_string(err) +
            " (path-based removable access UNVERIFIED on console; err=5 = brokered-path access denied)");
    if (listed)
    {
        for (const DirItem& it : items)
        {
            if (it.is_dir)
            {
                if (it.name == "PS3_GAME") continue; // root candidate already tried
                GameEntry e;
                if (classify_folder(root + "/" + it.name, it.name, GameSource::Usb, e, log))
                {
                    finish_entry(e, ls, true);
                    out.push_back(e);
                }
            }
            else if (ends_with_iso(it.name))
            {
                GameEntry e;
                e.title = stem_of(it.name);
                e.serial = "ISO";
                e.path = root + "/" + it.name;
                e.source = GameSource::Usb;
                e.kind = GameKind::Iso;
                e.size_bytes = it.size;
                out.push_back(e);
            }
        }
    }
    log_entries(out, 0, log);
    if (log)
        log("FRONTEND-USB-SCAN-DONE root=" + root +
            " entries=" + std::to_string(out.size()));
    return out;
}

} // namespace frontend
