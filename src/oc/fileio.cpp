#include <../../nrnconf.h>
/* /local/src/master/nrn/src/oc/fileio.cpp,v 1.34 1999/09/14 13:11:46 hines Exp */

#include <stdio.h>
#include <stdlib.h>
#include <cstdarg>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <locale>
#include <optional>
#include <regex>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include "ocmisc.h"
#include "hocstr.h"
#include "parse.hpp"
#include "hocparse.h"
#include <errno.h>
#include "nrnfilewrap.h"
#include <fmt/format.h>

extern char* neuron_home;

NrnFILEWrap* hoc_frin;
FILE* hoc_fout;

void hoc_stdout(void) {
    static int prev = -1;
    if (ifarg(1)) {
        FILE* f1;
        if (prev != -1) {
            hoc_execerror("stdout already switched", (char*) 0);
        }
        prev = dup(1);
        if (prev < 0) {
            hoc_execerror("Unable to backup stdout", (char*) 0);
        }
        f1 = fopen(gargstr(1), "wb");

        if (!f1) {
            hoc_execerror("Unable to open ", gargstr(1));
        }
        if (dup2(fileno(f1), 1) < 0) {
            hoc_execerror("Unable to attach stdout to ", gargstr(1));
        }
        fclose(f1);
    } else if (prev > -1) {
        if (dup2(prev, 1) < 0) {
            hoc_execerror("Unable to restore stdout", (char*) 0);
        }
        close(prev);
        prev = -1;
    }
    hoc_ret();
    hoc_pushx((double) fileno(stdout));
}

void hoc_ropen(void) /* open file for reading */
{
    double d;
    const char* fname;

    if (ifarg(1))
        fname = gargstr(1);
    else
        fname = "";
    d = 1.;
    if (!nrn_fw_eq(hoc_frin, stdin))
        IGNORE(nrn_fw_fclose(hoc_frin));
    hoc_frin = nrn_fw_set_stdin();
    if (fname[0] != 0) {
        if ((hoc_frin = nrn_fw_fopen(fname, "r")) == (NrnFILEWrap*) 0) {
            const char* retry;
            retry = expand_env_var(fname);
            if ((hoc_frin = nrn_fw_fopen(retry, "r")) == (NrnFILEWrap*) 0) {
                d = 0.;
                hoc_frin = nrn_fw_set_stdin();
            }
        }
    }
    errno = 0;
    hoc_ret();
    hoc_pushx(d);
}

void hoc_wopen(void) /* open file for writing */
{
    const char* fname;
    double d;

    if (ifarg(1))
        fname = gargstr(1);
    else
        fname = "";
    d = 1.;
    if (hoc_fout != stdout) {
        IGNORE(fclose(hoc_fout));
    }
    hoc_fout = stdout;
    if (fname[0] != 0) {
        if ((hoc_fout = fopen(expand_env_var(fname), "w")) == nullptr) {
            d = 0.;
            hoc_fout = stdout;
        }
    }
    errno = 0;
    hoc_ret();
    hoc_pushx(d);
}

const char* expand_env_var(const char* s) {
    static HocStr* hs;
    const char* cp1;
    char* cp2;
    int n;
    int begin = 1; /* only needed for mac when necessary to prepend a : */
    if (!hs) {
        hs = hocstr_create(256);
    }
    hocstr_resize(hs, strlen(s) + 2);
    for (cp1 = s, cp2 = hs->buf + begin; *cp1; ++cp1) {
        if (*cp1 == '$' && cp1[1] == '(') {
            char* cp3;
            char buf[200];
            cp1 += 2;
            for (cp3 = buf; *cp1 && *cp1 != ')'; ++cp1) {
                *cp3++ = *cp1;
                assert(cp3 - buf < 200);
            }
            if (*cp1) {
                *cp3 = '\0';
                if (strcmp(buf, "NEURONHOME") == 0) {
                    cp3 = neuron_home;
                } else {
                    cp3 = getenv(buf);
                }
                if (cp3) {
                    n = cp2 - hs->buf;
                    hocstr_resize(hs, n + strlen(cp3) + strlen(s) + 2);
                    cp2 = hs->buf + n;
                    while (*cp3) {
                        *cp2++ = *cp3++;
                    }
                }
            } else {
                --cp1;
            }
        } else {
            *cp2++ = *cp1;
        }
    }
    *cp2 = '\0';
    return hs->buf + begin;
}

size_t hoc_xopen_file_size_;
char* hoc_xopen_file_;

char* hoc_current_xopen(void) {
    return hoc_xopen_file_;
}

// read and execute a hoc program
int hoc_xopen1(const char* name, const char* rcs) {
    std::string fname{name};
    if (rcs) {
        if (rcs[0] != '\0') {
            // cmd ---> co -p{rcs} {fname} > {fname}-{rcs}
            // fname -> {fname}-{rcs}
            std::string cmd{"co -p"};
            cmd.append(rcs);
            cmd.append(1, ' ');
            cmd.append(fname);
            cmd.append(" > ");
            fname.append(1, '-');
            fname.append(rcs);
            cmd.append(fname);
            if (system(cmd.c_str()) != 0) {
                hoc_execerror(name, "\nreturned error in hoc_co system call");
            }
        }
    } else if (hoc_retrieving_audit()) {
        hoc_xopen_from_audit(fname.c_str());
        return 0;
    }
    auto const savpipflag = hoc_pipeflag;
    auto const savfin = hoc_fin;
    hoc_pipeflag = 0;

    errno = EINTR;
    while (errno == EINTR) {
        errno = 0;
        constexpr auto mode_str = "r";
        if (!(hoc_fin = nrn_fw_fopen(fname.c_str(), mode_str))) {
            fname = expand_env_var(fname.c_str());
            if (!(hoc_fin = nrn_fw_fopen(fname.c_str(), mode_str))) {
                hoc_fin = savfin;
                hoc_pipeflag = savpipflag;
                hoc_execerror("Can't open ", fname.c_str());
            }
        }
    }

    auto const save_lineno = hoc_lineno;
    hoc_lineno = 0;
    std::string savname{hoc_xopen_file_};
    if (fname.size() >= hoc_xopen_file_size_) {
        hoc_xopen_file_size_ = fname.size() + 100;
        hoc_xopen_file_ = static_cast<char*>(erealloc(hoc_xopen_file_, hoc_xopen_file_size_));
    }
    strcpy(hoc_xopen_file_, fname.c_str());
    if (hoc_fin) {
        hoc_audit_from_xopen1(fname.c_str(), rcs);
        IGNORE(hoc_xopen_run((Symbol*) 0, (char*) 0));
    }
    if (hoc_fin && !nrn_fw_eq(hoc_fin, stdin)) {
        IGNORE(nrn_fw_fclose(hoc_fin));
    }
    hoc_fin = savfin;
    hoc_pipeflag = savpipflag;
    if (rcs && rcs[0]) {
        unlink(fname.c_str());
    }
    hoc_xopen_file_[0] = '\0';
    hoc_lineno = save_lineno;
    strcpy(hoc_xopen_file_, savname.c_str());
    return 0;
}

void hoc_xopen(void) /* read and execute a hoc program */
{
    if (ifarg(2)) {
        hoc_xopen1(gargstr(1), gargstr(2));
    } else {
        hoc_xopen1(gargstr(1), 0);
    }
    hoc_ret();
    hoc_pushx(1.);
}

void hoc_Fprint(void) /* fprintf function */
{
    char* buf;
    double d;

    hoc_sprint1(&buf, 1);
    d = (double) fprintf(hoc_fout, "%s", buf);
    hoc_ret();
    hoc_pushx(d);
}

void hoc_PRintf(void) /* printf function */
{
    char* buf;
    double d;

    hoc_sprint1(&buf, 1);
    d = (int) strlen(buf);
    hoc_plprint(buf);
    fflush(stdout);
    hoc_ret();
    hoc_pushx(d);
}


void hoc_Sprint(void) /* sprintf_function */
{
    char** cpp;
    char* buf;
    /* This is not guaranteed safe since we may be pointing to double */
    cpp = hoc_pgargstr(1);
    hoc_sprint1(&buf, 2);
    hoc_assign_str(cpp, buf);
    hoc_ret();
    hoc_pushx(1.);
}

double hoc_scan(FILE* fi) {
    double d;
    char fs[256];

    for (;;) {
        if (fscanf(fi, "%255s", fs) == EOF) {
            hoc_execerror("EOF in fscan", (char*) 0);
        }
        if (fs[0] == 'i' || fs[0] == 'n' || fs[0] == 'I' || fs[0] == 'N') {
            continue;
        }
        if (sscanf(fs, "%lf", &d) == 1) {
            /* but if at end of line, leave at beginning of next*/
            if (fscanf(fi, "\n")) {
                ;
            } /* ignore return value */
            break;
        }
    }
    return d;
}

double hoc_fw_scan(NrnFILEWrap* fi) {
    double d;
    char fs[256];

    for (;;) {
        if (nrn_fw_fscanf(fi, "%255s", fs) == EOF) {
            hoc_execerror("EOF in fscan", (char*) 0);
        }
        if (fs[0] == 'i' || fs[0] == 'n' || fs[0] == 'I' || fs[0] == 'N') {
            continue;
        }
        if (sscanf(fs, "%lf", &d) == 1) {
            /* but if at end of line, leave at beginning of next*/
            nrnignore = nrn_fw_fscanf(fi, "\n");
            break;
        }
    }
    return d;
}

void hoc_Fscan(void) /* read a number from input file */
{
    double d;
    NrnFILEWrap* fi;

    if (nrn_fw_eq(hoc_frin, stdin)) {
        fi = hoc_fin;
    } else {
        fi = hoc_frin;
    }
    d = hoc_fw_scan(fi);
    hoc_ret();
    hoc_pushx(d);
}

void hoc_Getstr(void) /* read a line (or word) from input file */
{
    char* buf;
    char** cpp;
    NrnFILEWrap* fi;
    int word = 0;
    if (nrn_fw_eq(hoc_frin, stdin)) {
        fi = hoc_fin;
    } else {
        fi = hoc_frin;
    }
    cpp = hoc_pgargstr(1);
    if (ifarg(2)) {
        word = (int) chkarg(2, 0., 1.);
    }
    if (word) {
        buf = hoc_tmpbuf->buf;
        if (nrn_fw_fscanf(fi, "%s", buf) != 1) {
            hoc_execerror("EOF in getstr", (char*) 0);
        }
    } else {
        if ((buf = fgets_unlimited(hoc_tmpbuf, fi)) == (char*) 0) {
            hoc_execerror("EOF in getstr", (char*) 0);
        }
    }
    hoc_assign_str(cpp, buf);
    hoc_ret();
    hoc_pushx((double) strlen(buf));
}

void hoc_sprint1(char** ppbuf, int argn) { /* convert args to right type for conversion */
    /* argn is argument number where format is */
    static HocStr* hs;
    char *pfmt, *pfrag, frag[120];
    int convflag, lflag, didit;  //, hoc_argtype();
    char *fmt, *cp;

    if (!hs) {
        hs = hocstr_create(512);
    }
    fmt = gargstr(argn++);
    convflag = lflag = didit = 0;
    auto* pbuf = hs->buf;
    auto pbuf_size = hs->size + 1;
    pfrag = frag;
    *pfrag = 0;
    *pbuf = 0;

    auto const resize = [&pbuf, &pbuf_size](HocStr* hs, std::size_t extra_size) {
        auto const n = pbuf - hs->buf;
        auto const new_size = n + extra_size;
        hocstr_resize(hs, new_size);
        pbuf = hs->buf + n;
        pbuf_size = new_size + 1 - n;
    };
    for (pfmt = fmt; *pfmt; pfmt++) {
        *pfrag++ = *pfmt;
        *pfrag = 0;
        if (convflag) {
            switch (*pfmt) {
            case 'l':
                lflag += 1;
                break;

            case 'd':
            case 'o':
            case 'x':
                if (lflag) {
                    if (lflag == 1) {
                        pfrag[1] = pfrag[0];
                        pfrag[0] = pfrag[-1];
                        pfrag[-1] = 'l';
                    }
                    std::snprintf(pbuf, pbuf_size, frag, (long long) *getarg(argn));
                } else {
                    std::snprintf(pbuf, pbuf_size, frag, (int) *getarg(argn));
                }
                didit = 1;
                break;

            case 'c':
                std::snprintf(pbuf, pbuf_size, frag, (char) *getarg(argn));
                didit = 1;
                break;

            case 'f':
            case 'e':
            case 'g':
                std::snprintf(pbuf, pbuf_size, frag, *getarg(argn));
                didit = 1;
                break;

            case 's':
                if (hoc_is_object_arg(argn)) {
                    cp = hoc_object_name(*hoc_objgetarg(argn));
                } else {
                    cp = gargstr(argn);
                }
                resize(hs, std::strlen(cp) + 100);
                std::snprintf(pbuf, pbuf_size, frag, cp);
                didit = 1;
                break;

            case '%':
                pfrag[-1] = 0;
                std::strncpy(pbuf, frag, pbuf_size);
                assert(pbuf[pbuf_size - 1] == '\0');
                didit = 1;
                argn--; /* an arg was not consumed */
                break;

            default:
                break;
            }
        } else if (*pfmt == '%') {
            convflag = 1;
        } else if (pfrag - frag > 100) {
            resize(hs, std::strlen(frag) + 100);
            std::snprintf(pbuf, pbuf_size, "%s", frag);
            pfrag = frag;
            *pfrag = 0;
            while (*pbuf) {
                ++pbuf;
                --pbuf_size;
            }
        }

        if (didit) {
            argn++;
            lflag = 0;
            convflag = 0;
            didit = 0;
            pfrag = frag;
            *pfrag = 0;
            while (*pbuf) {
                ++pbuf;
                --pbuf_size;
            }
            resize(hs, 100);
        }
    }
    if (pfrag != frag)
        std::snprintf(pbuf, pbuf_size, "%s", frag);
    *ppbuf = hs->buf;
}


// Split a string containing an env variable into multiple paths (OS-specific)
// and return a container with the results
static auto split_paths(const std::string& input) {
    // separator for items in env variables
#if defined(WIN32)
    static constexpr auto os_pathsep = ";";
#else
    static constexpr auto os_pathsep = ":";
#endif

    std::vector<std::string> result;
    std::size_t start = 0;
    std::size_t end = 0;

    while ((end = input.find(os_pathsep, start)) != std::string::npos) {
        if (end > start) {
            result.push_back(input.substr(start, end - start));
        }
        start = end + 1;
    }

    // Add the last segment if it's non-empty
    if (start < input.size()) {
        result.push_back(input.substr(start));
    }

    return result;
}

// return the default search paths for loading files
static auto default_search_paths() {
    auto result = std::vector<std::string>({"."});

    // insert hoc paths (if any)
    auto hoc_library_path = getenv("HOC_LIBRARY_PATH");
    if (hoc_library_path) {
        auto paths = split_paths(std::string(hoc_library_path));
        std::copy(begin(paths), end(paths), back_inserter(result));
    }

    // insert home path
    auto home_path = fmt::format("{}/lib/hoc", std::string(neuron_home));
    result.push_back(home_path);

    return result;
}

// look for regex ``pattern`` in ``paths`` (non-recursively);
// if there is no match, return empty value
// if there is a match, return the path of the first file containing the match
static std::optional<std::string> search_hoc_files_regex(const std::regex& pattern,
                                                         const std::vector<std::string>& paths) {
    namespace fs = std::filesystem;
    for (const auto& path: paths) {
        // construct a list containing names of, in this order:
        // - `.oc` files (sorted according to locale)
        // - `.hoc` files (sorted according to locale)
        std::vector<std::string> paths_oc;
        std::vector<std::string> paths_hoc;
        for (const auto& entry: fs::directory_iterator(path)) {
            if (entry.is_regular_file() && entry.path().extension() == ".oc") {
                paths_oc.push_back(entry.path().string());
            } else if (entry.is_regular_file() && entry.path().extension() == ".hoc") {
                paths_hoc.push_back(entry.path().string());
            }
        }
        std::sort(paths_oc.begin(), paths_oc.end(), std::locale());
        std::sort(paths_hoc.begin(), paths_hoc.end(), std::locale());

        std::vector<std::string> result;
        std::merge(paths_oc.begin(),
                   paths_oc.end(),
                   paths_hoc.begin(),
                   paths_hoc.end(),
                   back_inserter(result));

        for (const auto& entry: result) {
            auto file = std::ifstream(entry);
            if (!file.is_open())
                continue;

            std::string line;
            while (std::getline(file, line)) {
                if (std::regex_search(line, pattern)) {
                    return entry;
                }
            }
        }
    }
    return {};
}

static int hoc_Load_file(int, const char*);

static void hoc_load(const char* stype) {
    int i = 1;

    while (ifarg(i)) {
        const char* s = gargstr(i);
        ++i;
        const Symbol* sym = hoc_lookup(s);
        if (!sym || sym->type == UNDEF) {
            auto pattern = std::regex(fmt::format("^{} {}", stype, s));
            auto file = search_hoc_files_regex(pattern, default_search_paths());
            if (file) {
                fprintf(stderr, "Getting %s from %s\n", s, file->c_str());
                hoc_Load_file(0, file->c_str());
            } else {
                fprintf(stderr, "Couldn't find a file that declares %s\n", s);
            }
        }
    }
}

void hoc_load_proc(void) {
    hoc_load("proc");
    hoc_ret();
    hoc_pushx(1.);
}
void hoc_load_func(void) {
    hoc_load("func");
    hoc_ret();
    hoc_pushx(1.);
}
void hoc_load_template(void) {
    hoc_load("begintemplate");
    hoc_ret();
    hoc_pushx(1.);
}

void hoc_load_file(void) {
    int iarg = 1;
    int i = 0;
    if (hoc_is_double_arg(iarg)) {
        i = (int) chkarg(iarg, 0., 1.);
        iarg = 2;
    }
    if (!ifarg(iarg + 1) || !hoc_lookup(gargstr(iarg + 1))) {
        i = hoc_Load_file(i, gargstr(iarg));
    }
    hoc_ret();
    hoc_pushx((double) i);
}

static constexpr auto hoc_load_file_size_ = 1024;
static int hoc_Load_file(int always, const char* name) {
    /*
      if always is 0 then
      xopen only if file of that name not already loaded with one of
        the load_xxx functions
        and search in the current, $HOC_LIBRARY_PATH,
        $NEURONHOME/lib/hoc directories (in that order) for
        the file if there is no directory prefix.
        Temporarily change to the directory containing the file so
        that it can xopen files relative to its location.
    */
    static std::vector<std::string> loaded;
    int b, is_loaded;
    int goback;
    char expname[hoc_load_file_size_];
    const char* base;
    char path[hoc_load_file_size_], old[hoc_load_file_size_];
    char fname[hoc_load_file_size_], cmd[hoc_load_file_size_ + 50];
    FILE* f;

    old[0] = '\0';
    goback = 0;
    /* has the file already been loaded */
    is_loaded = 0;

    for (const std::string& q: loaded) {
        if (q == name) {
            if (!always) {
                return 1;
            } else {
                is_loaded = 1;
            }
        }
    }

    /* maybe the name already has an explicit path */
    expname[hoc_load_file_size_ - 1] = '\0';
    strncpy(expname, expand_env_var(name), hoc_load_file_size_);
    assert(expname[hoc_load_file_size_ - 1] == '\0');
    name = expname;
    if ((base = strrchr(name, '/')) != NULL) {
        strncpy(path, name, base - name);
        path[base - name] = '\0';
        ++base;
        f = fopen(name, "r");
    } else {
        base = name;
        path[0] = '\0';
        /* otherwise find the file in the default directories */
        f = fopen(base, "r"); /* cwd */
        if (!f) {             /* try HOC_LIBRARY_PATH */
            char* hlp;
            hlp = getenv("HOC_LIBRARY_PATH");
            while (hlp && *hlp) {
                char* cp = strchr(hlp, ':');
                if (!cp) {
                    cp = strchr(hlp, ' ');
                }
                if (!cp) {
                    cp = hlp + strlen(hlp);
                }
                assert(cp - hlp < hoc_load_file_size_);
                strncpy(path, hlp, cp - hlp);
                path[cp - hlp] = '\0';
                if (*cp) {
                    hlp = cp + 1;
                } else {
                    hlp = 0;
                }
                if (path[0]) {
                    nrn_assert(snprintf(fname, hoc_load_file_size_, "%s/%s", path, base) <
                               hoc_load_file_size_);
                    f = fopen(expand_env_var(fname), "r");
                    if (f) {
                        break;
                    }
                } else {
                    break;
                }
            }
        }
        if (!f) { /* try NEURONHOME/lib/hoc */
            Sprintf(path, "$(NEURONHOME)/lib/hoc");
            assert(strlen(path) + strlen(base) + 1 < hoc_load_file_size_);
            nrn_assert(snprintf(fname, hoc_load_file_size_, "%s/%s", path, base) <
                       hoc_load_file_size_);
            f = fopen(expand_env_var(fname), "r");
        }
    }
    /* add the name to the list of loaded packages */
    if (f) {
        if (!is_loaded) {
            loaded.push_back(name);
        }
        b = 1;
    } else {
        b = 0;
        hoc_warning("Couldn't find:", name);
        path[0] = '\0';
    }
    /* change to the right directory*/
    if (b && path[0]) {
        goback = (getcwd(old, 1000) != 0);
        errno = 0;
        if (chdir(expand_env_var(path)) == -1) {
            hoc_warning("Couldn't change directory to:", path);
            path[0] = '\0';
            b = 0;
        }
        /*printf("load_file cd to %s\n", path);*/
    }
    /* xopen the file */
    if (b) {
        /*printf("load_file xopen %s\n", base);*/
        nrn_assert(strlen(base) < hoc_load_file_size_);
        snprintf(cmd,
                 hoc_load_file_size_ + 50,
                 "hoc_ac_ = execute1(\"{xopen(\\\"%s\\\")}\")\n",
                 base);
        b = hoc_oc(cmd);
        b = (int) hoc_ac_;
        if (!b) {
            hoc_execerror("hoc_Load_file", base);
        }
    }
    /* change back */
    if (path[0] && goback) {
        if (hoc_chdir(old) == -1) {
            hoc_warning("Couldn't change directory back to:", old);
            b = 0;
        }
        /*printf("load_file cd back to %s\n", old);*/
    }

    return b;
}
char* hoc_back2forward(char*);
void hoc_getcwd(void) {
    int len;
    static char* buf;
    if (!buf) {
        buf = static_cast<char*>(emalloc(hoc_load_file_size_));
    }
    if (!getcwd(buf, hoc_load_file_size_)) {
        hoc_execerror("getcwd failed. Perhaps the path length is > hoc_load_file_size_", (char*) 0);
    }
#if defined(WIN32)
    { strcpy(buf, hoc_back2forward(buf)); }
#endif
    len = strlen(buf);
    if (buf[len - 1] != '/') {
        buf[len] = '/';
        buf[len + 1] = '\0';
    }
    hoc_ret();
    hoc_pushstr(&buf);
}

void hoc_machine_name(void) {
#if !defined(WIN32)
    /*----- functions called -----*/
    /*----- local  variables -----*/
    char buf[20];

    gethostname(buf, 20);
    hoc_assign_str(hoc_pgargstr(1), buf);
#endif
    hoc_ret();
    hoc_pushx(0.);
}

int hoc_chdir(const char* path) {
    return chdir(expand_env_var(path));
}

void hoc_Chdir(void) {
    int i = hoc_chdir(gargstr(1));
    hoc_ret();
    hoc_pushx((double) i);
}

int nrn_is_python_extension;
int (*nrnpy_pr_stdoe_callback)(int, char*);
static int (*nrnpy_pass_callback)();

extern "C" void nrnpy_set_pr_etal(int (*cbpr_stdoe)(int, char*), int (*cbpass)()) {
    nrnpy_pr_stdoe_callback = cbpr_stdoe;
    nrnpy_pass_callback = cbpass;
}

void nrnpy_pass() {
    if (nrnpy_pass_callback) {
        if ((*nrnpy_pass_callback)() != 1) {
            hoc_execerror("nrnpy_pass", nullptr);
        }
    }
}

/** printf style specification of hoc_execerror message. (512 char limit) **/
[[noreturn]] void hoc_execerr_ext(const char* fmt, ...) {
    int size;  // vsnprintf returns -1 on error.
    va_list ap;

    // determine the message size
    va_start(ap, fmt);
    size = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);

    if (size >= 0) {
        constexpr size_t maxsize = 512;
        char s[maxsize + 1];
        va_start(ap, fmt);
        size = vsnprintf(s, maxsize, fmt, ap);
        va_end(ap);
        if (size >= 0) {
            s[maxsize] = '\0';  // truncate if too long
            hoc_execerror(s, NULL);
        }
    }
    hoc_execerror("hoc_execerr_ext failure with format:", fmt);
}
