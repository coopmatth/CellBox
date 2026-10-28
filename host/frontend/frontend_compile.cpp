// frontend_compile.cpp - see frontend_compile.h. The directory-walk
// helpers repeat frontend_library.cpp's FromApp pattern (each native
// TU in this line carries its own copy; no shared host utility TU
// exists).
#include "frontend_compile.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <fileapifromapp.h>

#include <algorithm>
#include <cctype>

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
    std::string name;
    bool is_dir = false;
    std::uint64_t size = 0;
};

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

std::string lower_of(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Recursive module walk. rel is "" at the root. Budget/depth caps
// mirror frontend_library's folder_size so a huge tree cannot stall
// the menu.
void collect_modules(const std::string& dir, const std::string& rel, int depth,
                     unsigned& budget, std::vector<CompileModule>& out)
{
    if (depth > 8 || budget == 0) return;
    std::vector<DirItem> items;
    unsigned long err = 0;
    if (!list_dir(dir, items, err)) return;
    for (const DirItem& it : items)
    {
        if (budget == 0) break;
        --budget;
        const std::string childRel = rel.empty() ? it.name : rel + "/" + it.name;
        if (it.is_dir)
            collect_modules(dir + "/" + it.name, childRel, depth + 1, budget, out);
        else if (is_module_filename(it.name))
            out.push_back(CompileModule{childRel, it.size});
    }
}

void count_files(const std::string& dir, int depth, unsigned& budget, CacheStats& st)
{
    if (depth > 6 || budget == 0) return;
    std::vector<DirItem> items;
    unsigned long err = 0;
    if (!list_dir(dir, items, err)) return;
    for (const DirItem& it : items)
    {
        if (budget == 0) break;
        --budget;
        if (it.is_dir) count_files(dir + "/" + it.name, depth + 1, budget, st);
        else { ++st.files; st.bytes += it.size; }
    }
}

} // namespace

bool is_module_filename(const std::string& name)
{
    // The core's precompile scan (PPUThread.cpp) excludes this one
    // utility SPRX by name; mirror that so the UI count matches the
    // core's queue.
    if (lower_of(name) == "libfs_utility_init.sprx") return false;
    const size_t dot = name.find_last_of('.');
    if (dot == std::string::npos) return false;
    const std::string ext = lower_of(name.substr(dot));
    return ext == ".sprx" || ext == ".prx" || ext == ".elf" ||
           ext == ".self" || ext == ".mself";
}

ModuleEnum enumerate_modules(const GameEntry& e, const LogFn& log)
{
    ModuleEnum out;
    if (e.kind != GameKind::Folder)
    {
        if (log) log("FRONTEND-COMPILE-ENUM kind=iso: no host dir to enumerate (ISO compile route is a boot pass)");
        return out;
    }
    const std::string p = slash(e.path);
    const std::string ebootSuffix = "/USRDIR/EBOOT.BIN";
    std::string dir;
    if (p.size() > ebootSuffix.size() &&
        lower_of(p.substr(p.size() - ebootSuffix.size())) == lower_of(ebootSuffix))
    {
        dir = p.substr(0, p.size() - ebootSuffix.size()); // shape B: path IS the EBOOT
    }
    else if (is_file_utf8(p + "/PS3_GAME/USRDIR/EBOOT.BIN"))
    {
        dir = p + "/PS3_GAME"; // shape A: disc root
    }
    else if (is_file_utf8(p + "/USRDIR/EBOOT.BIN"))
    {
        dir = p; // PS3_GAME dir handed over directly
    }
    if (dir.empty())
    {
        if (log) log("FRONTEND-COMPILE-ENUM no EBOOT.BIN located under path=" + p +
                     " (compile unavailable for this entry)");
        return out;
    }
    out.compile_dir = dir;
    std::uint64_t ebootSize = 0;
    out.eboot_present = is_file_utf8(dir + "/USRDIR/EBOOT.BIN", &ebootSize);
    if (!out.eboot_present)
    {
        if (log) log("FRONTEND-COMPILE-ENUM compile dir resolved but EBOOT missing dir=" + dir);
        return out;
    }
    out.ok = true;
    out.modules.push_back(CompileModule{"USRDIR/EBOOT.BIN", ebootSize});
    std::vector<CompileModule> rest;
    unsigned budget = 20000;
    collect_modules(dir, "", 0, budget, rest);
    std::sort(rest.begin(), rest.end(),
              [](const CompileModule& a, const CompileModule& b) { return a.rel < b.rel; });
    for (const CompileModule& m : rest) out.modules.push_back(m);
    for (const CompileModule& m : out.modules) out.total_bytes += m.size;
    if (log)
    {
        log("FRONTEND-COMPILE-DIR dir=" + dir + " source=" + game_source_name(e.source));
        log("FRONTEND-COMPILE-MODULES count=" + std::to_string(out.modules.size()) +
            " bytes=" + std::to_string(out.total_bytes) +
            " (EBOOT.BIN compiles as the main module; .MSELF containers are expanded by the core)");
        const size_t shown = out.modules.size() < 40 ? out.modules.size() : 40;
        for (size_t i = 0; i < shown; ++i)
            log("FRONTEND-COMPILE-MODULE idx=" + std::to_string(i) +
                " size=" + std::to_string(out.modules[i].size) +
                " rel=" + out.modules[i].rel);
        if (shown < out.modules.size())
            log("FRONTEND-COMPILE-MODULE ... +" +
                std::to_string(out.modules.size() - shown) + " more (log cap)");
    }
    return out;
}

CacheStats ppu_cache_stats(const std::string& localStateUtf8,
                           const std::string& serial, const LogFn& log)
{
    CacheStats st;
    const std::string ls = slash(localStateUtf8);
    const bool serialOk = !serial.empty() && serial != "ISO" && serial != "-";
    st.dir = serialOk ? ls + "/cache/" + serial : ls + "/cache";
    st.dir_exists = is_dir_utf8(st.dir);
    if (st.dir_exists)
    {
        unsigned budget = 20000;
        count_files(st.dir, 0, budget, st);
    }
    if (log)
        log("FRONTEND-COMPILE-CACHE dir=" + st.dir +
            " exists=" + std::string(st.dir_exists ? "1" : "0") +
            " files=" + std::to_string(st.files) +
            " bytes=" + std::to_string(st.bytes) +
            (serialOk ? " scope=per-title" : " scope=whole-cache (entry serial is a placeholder)"));
    return st;
}

ShaderCacheFile shader_cache_file(const std::string& localStateUtf8)
{
    ShaderCacheFile f;
    f.path = slash(localStateUtf8) + "/d3d12_shader_cache.bin";
    f.present = is_file_utf8(f.path, &f.bytes);
    if (!f.present) f.bytes = 0;
    return f;
}

std::string current_module_from_notice(const std::string& notice)
{
    // Core notices: "Trying to load: <path>" (per precompile file) and
    // "Trying to load binary: <path>" (the main EBOOT).
    if (notice.find("Trying to load") != 0) return "";
    const size_t colon = notice.find(": ");
    if (colon == std::string::npos) return "";
    std::string path = notice.substr(colon + 2);
    const size_t sep = path.find_last_of("/\\");
    std::string name = sep == std::string::npos ? path : path.substr(sep + 1);
    while (!name.empty() && (name.back() == ' ' || name.back() == '\r' || name.back() == '\n'))
        name.pop_back();
    if (name.size() > 64) name = name.substr(0, 61) + "...";
    return name;
}

} // namespace frontend
