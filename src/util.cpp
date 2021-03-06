// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <util.h>

#include <chainparamsbase.h>
#include <random.h>
#include <serialize.h>
#include <utilstrencodings.h>

#include <stdarg.h>

#if (defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__DragonFly__))
#include <pthread.h>
#include <pthread_np.h>
#endif

#ifndef WIN32

#include <algorithm>
#include <fcntl.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/stat.h>

#else

#ifdef _MSC_VER
#pragma warning(disable:4786)
#pragma warning(disable:4804)
#pragma warning(disable:4805)
#pragma warning(disable:4717)
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <io.h> /* for _commit */
#include <shlobj.h>
#endif

#ifdef HAVE_SYS_PRCTL_H
#include <sys/prctl.h>
#endif

#ifdef HAVE_MALLOPT_ARENA_MAX
#include <malloc.h>
#endif

#include <string>
#include <unordered_set>

#include <boost/interprocess/sync/file_lock.hpp>
#include <boost/thread.hpp>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <openssl/conf.h>
#include <thread>

// Application startup time (used for uptime calculation)
const int64_t nStartupTime = GetTime();

const char * const BITCOIN_CONF_FILENAME = "bitcoin.conf";
const char * const BITCOIN_RW_CONF_FILENAME = "bitcoin_rw.conf";
const char * const BITCOIN_PID_FILENAME = "bitcoind.pid";

ArgsManager gArgs;

CTranslationInterface translationInterface;

/** Init OpenSSL library multithreading support */
static std::unique_ptr<CCriticalSection[]> ppmutexOpenSSL;
void locking_callback(int mode, int i, const char* file, int line) NO_THREAD_SAFETY_ANALYSIS
{
    if (mode & CRYPTO_LOCK) {
        ENTER_CRITICAL_SECTION(ppmutexOpenSSL[i]);
    } else {
        LEAVE_CRITICAL_SECTION(ppmutexOpenSSL[i]);
    }
}

// Singleton for wrapping OpenSSL setup/teardown.
class CInit
{
public:
    CInit()
    {
        // Init OpenSSL library multithreading support
        ppmutexOpenSSL.reset(new CCriticalSection[CRYPTO_num_locks()]);
        CRYPTO_set_locking_callback(locking_callback);

        // OpenSSL can optionally load a config file which lists optional loadable modules and engines.
        // We don't use them so we don't require the config. However some of our libs may call functions
        // which attempt to load the config file, possibly resulting in an exit() or crash if it is missing
        // or corrupt. Explicitly tell OpenSSL not to try to load the file. The result for our libs will be
        // that the config appears to have been loaded and there are no modules/engines available.
        OPENSSL_no_config();

#ifdef WIN32
        // Seed OpenSSL PRNG with current contents of the screen
        RAND_screen();
#endif

        // Seed OpenSSL PRNG with performance counter
        RandAddSeed();
    }
    ~CInit()
    {
        // Securely erase the memory used by the PRNG
        RAND_cleanup();
        // Shutdown OpenSSL library multithreading support
        CRYPTO_set_locking_callback(nullptr);
        // Clear the set of locks now to maintain symmetry with the constructor.
        ppmutexOpenSSL.reset();
    }
}
instance_of_cinit;

/** A map that contains all the currently held directory locks. After
 * successful locking, these will be held here until the global destructor
 * cleans them up and thus automatically unlocks them, or ReleaseDirectoryLocks
 * is called.
 */
static std::map<std::string, std::unique_ptr<boost::interprocess::file_lock>> dir_locks;
/** Mutex to protect dir_locks. */
static std::mutex cs_dir_locks;

bool LockDirectory(const fs::path& directory, const std::string lockfile_name, bool probe_only)
{
    std::lock_guard<std::mutex> ulock(cs_dir_locks);
    fs::path pathLockFile = directory / lockfile_name;

    // If a lock for this directory already exists in the map, don't try to re-lock it
    if (dir_locks.count(pathLockFile.string())) {
        return true;
    }

    // Create empty lock file if it doesn't exist.
    FILE* file = fsbridge::fopen(pathLockFile, "a");
    if (file) fclose(file);

    try {
        auto lock = MakeUnique<boost::interprocess::file_lock>(pathLockFile.string().c_str());
        if (!lock->try_lock()) {
            return false;
        }
        if (!probe_only) {
            // Lock successful and we're not just probing, put it into the map
            dir_locks.emplace(pathLockFile.string(), std::move(lock));
        }
    } catch (const boost::interprocess::interprocess_exception& e) {
        return error("Error while attempting to lock directory %s: %s", directory.string(), e.what());
    }
    return true;
}

void ReleaseDirectoryLocks()
{
    std::lock_guard<std::mutex> ulock(cs_dir_locks);
    dir_locks.clear();
}

bool DirIsWritable(const fs::path& directory)
{
    fs::path tmpFile = directory / fs::unique_path();

    FILE* file = fsbridge::fopen(tmpFile, "a");
    if (!file) return false;

    fclose(file);
    remove(tmpFile);

    return true;
}

/**
 * Interpret a string argument as a boolean.
 *
 * The definition of atoi() requires that non-numeric string values like "foo",
 * return 0. This means that if a user unintentionally supplies a non-integer
 * argument here, the return value is always false. This means that -foo=false
 * does what the user probably expects, but -foo=true is well defined but does
 * not do what they probably expected.
 *
 * The return value of atoi() is undefined when given input not representable as
 * an int. On most systems this means string value between "-2147483648" and
 * "2147483647" are well defined (this method will return true). Setting
 * -txindex=2147483648 on most systems, however, is probably undefined.
 *
 * For a more extensive discussion of this topic (and a wide range of opinions
 * on the Right Way to change this code), see PR12713.
 */
static bool InterpretBool(const std::string& strValue)
{
    if (strValue.empty())
        return true;
    return (atoi(strValue) != 0);
}

/** Internal helper functions for ArgsManager */
class ArgsManagerHelper {
public:
    typedef std::map<std::string, std::vector<std::string>> MapArgs;

    /** Determine whether to use config settings in the default section,
     *  See also comments around ArgsManager::ArgsManager() below. */
    static inline bool UseDefaultSection(const ArgsManager& am, const std::string& arg)
    {
        return (am.m_network == CBaseChainParams::MAIN || am.m_network_only_args.count(arg) == 0);
    }

    /** Convert regular argument into the network-specific setting */
    static inline std::string NetworkArg(const ArgsManager& am, const std::string& arg)
    {
        assert(arg.length() > 1 && arg[0] == '-');
        return "-" + am.m_network + "." + arg.substr(1);
    }

    /** Find arguments in a map and add them to a vector */
    static inline void AddArgs(std::vector<std::string>& res, const MapArgs& map_args, const std::string& arg)
    {
        auto it = map_args.find(arg);
        if (it != map_args.end()) {
            res.insert(res.end(), it->second.begin(), it->second.end());
        }
    }

    /** Return true/false if an argument is set in a map, and also
     *  return the first (or last) of the possibly multiple values it has
     */
    static inline std::pair<bool,std::string> GetArgHelper(const MapArgs& map_args, const std::string& arg, bool getLast = false)
    {
        auto it = map_args.find(arg);

        if (it == map_args.end() || it->second.empty()) {
            return std::make_pair(false, std::string());
        }

        if (getLast) {
            return std::make_pair(true, it->second.back());
        } else {
            return std::make_pair(true, it->second.front());
        }
    }

    /* Get the string value of an argument, returning a pair of a boolean
     * indicating the argument was found, and the value for the argument
     * if it was found (or the empty string if not found).
     */
    static inline std::pair<bool,std::string> GetArg(const ArgsManager &am, const std::string& arg)
    {
        LOCK(am.cs_args);
        std::pair<bool,std::string> found_result(false, std::string());

        // We pass "true" to GetArgHelper in order to return the last
        // argument value seen from the command line (so "bitcoind -foo=bar
        // -foo=baz" gives GetArg(am,"foo")=={true,"baz"}
        found_result = GetArgHelper(am.m_override_args, arg, true);
        if (found_result.first) {
            return found_result;
        }

        // But in contrast we return the first argument seen in a config file,
        // so "foo=bar \n foo=baz" in the config file gives
        // GetArg(am,"foo")={true,"bar"}
        if (!am.m_network.empty()) {
            found_result = GetArgHelper(am.m_config_args, NetworkArg(am, arg));
            if (found_result.first) {
                return found_result;
            }
        }

        if (UseDefaultSection(am, arg)) {
            found_result = GetArgHelper(am.m_config_args, arg);
            if (found_result.first) {
                return found_result;
            }
        }

        return found_result;
    }

    /* Special test for -testnet and -regtest args, because we
     * don't want to be confused by craziness like "[regtest] testnet=1"
     */
    static inline bool GetNetBoolArg(const ArgsManager &am, const std::string& net_arg)
    {
        std::pair<bool,std::string> found_result(false,std::string());
        found_result = GetArgHelper(am.m_override_args, net_arg, true);
        if (!found_result.first) {
            found_result = GetArgHelper(am.m_config_args, net_arg, true);
            if (!found_result.first) {
                return false; // not set
            }
        }
        return InterpretBool(found_result.second); // is set, so evaluate
    }
};

/**
 * Interpret -nofoo as if the user supplied -foo=0.
 *
 * This method also tracks when the -no form was supplied, and if so,
 * checks whether there was a double-negative (-nofoo=0 -> -foo=1).
 *
 * If there was not a double negative, it removes the "no" from the key,
 * and returns true, indicating the caller should clear the args vector
 * to indicate a negated option.
 *
 * If there was a double negative, it removes "no" from the key, sets the
 * value to "1" and returns false.
 *
 * If there was no "no", it leaves key and value untouched and returns
 * false.
 *
 * Where an option was negated can be later checked using the
 * IsArgNegated() method. One use case for this is to have a way to disable
 * options that are not normally boolean (e.g. using -nodebuglogfile to request
 * that debug log output is not sent to any file at all).
 */
static bool InterpretNegatedOption(std::string& key, std::string& val)
{
    assert(key[0] == '-');

    size_t option_index = key.find('.');
    if (option_index == std::string::npos) {
        option_index = 1;
    } else {
        ++option_index;
    }
    if (key.substr(option_index, 2) == "no") {
        bool bool_val = InterpretBool(val);
        key.erase(option_index, 2);
        if (!bool_val ) {
            // Double negatives like -nofoo=0 are supported (but discouraged)
            LogPrintf("Warning: parsed potentially confusing double-negative %s=%s\n", key, val);
            val = "1";
        } else {
            return true;
        }
    }
    return false;
}

ArgsManager::ArgsManager() :
    /* These options would cause cross-contamination if values for
     * mainnet were used while running on regtest/testnet (or vice-versa).
     * Setting them as section_only_args ensures that sharing a config file
     * between mainnet and regtest/testnet won't cause problems due to these
     * parameters by accident. */
    m_network_only_args{
      "-addnode", "-connect",
      "-port", "-bind",
      "-rpcport", "-rpcbind",
      "-wallet",
    }
{
    // nothing to do
}

void ArgsManager::WarnForSectionOnlyArgs()
{
    // if there's no section selected, don't worry
    if (m_network.empty()) return;

    // if it's okay to use the default section for this network, don't worry
    if (m_network == CBaseChainParams::MAIN) return;

    for (const auto& arg : m_network_only_args) {
        std::pair<bool, std::string> found_result;

        // if this option is overridden it's fine
        found_result = ArgsManagerHelper::GetArgHelper(m_override_args, arg);
        if (found_result.first) continue;

        // if there's a network-specific value for this option, it's fine
        found_result = ArgsManagerHelper::GetArgHelper(m_config_args, ArgsManagerHelper::NetworkArg(*this, arg));
        if (found_result.first) continue;

        // if there isn't a default value for this option, it's fine
        found_result = ArgsManagerHelper::GetArgHelper(m_config_args, arg);
        if (!found_result.first) continue;

        // otherwise, issue a warning
        LogPrintf("Warning: Config setting for %s only applied on %s network when in [%s] section.\n", arg, m_network, m_network);
    }
}

void ArgsManager::SelectConfigNetwork(const std::string& network)
{
    m_network = network;
}

bool ArgsManager::ParseParameters(int argc, const char* const argv[], std::string& error)
{
    LOCK(cs_args);
    m_override_args.clear();

    for (int i = 1; i < argc; i++) {
        std::string key(argv[i]);
        std::string val;
        size_t is_index = key.find('=');
        if (is_index != std::string::npos) {
            val = key.substr(is_index + 1);
            key.erase(is_index);
        }
#ifdef WIN32
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        if (key[0] == '/')
            key[0] = '-';
#endif

        if (key[0] != '-')
            break;

        // Transform --foo to -foo
        if (key.length() > 1 && key[1] == '-')
            key.erase(0, 1);

        // Check for -nofoo
        if (InterpretNegatedOption(key, val)) {
            m_override_args[key].clear();
        } else {
            m_override_args[key].push_back(val);
        }

        // Check that the arg is known
        if (!(IsSwitchChar(key[0]) && key.size() == 1)) {
            if (!IsArgKnown(key)) {
                error = strprintf("Invalid parameter %s", key.c_str());
                return false;
            }
        }
    }

    // we do not allow -includeconf from command line, so we clear it here
    auto it = m_override_args.find("-includeconf");
    if (it != m_override_args.end()) {
        if (it->second.size() > 0) {
            for (const auto& ic : it->second) {
                error += "-includeconf cannot be used from commandline; -includeconf=" + ic + "\n";
            }
            return false;
        }
    }
    return true;
}

bool ArgsManager::IsArgKnown(const std::string& key) const
{
    size_t option_index = key.find('.');
    std::string arg_no_net;
    if (option_index == std::string::npos) {
        arg_no_net = key;
    } else {
        arg_no_net = std::string("-") + key.substr(option_index + 1, std::string::npos);
    }

    for (const auto& arg_map : m_available_args) {
        if (arg_map.second.count(arg_no_net)) return true;
    }
    return false;
}

std::vector<std::string> ArgsManager::GetArgs(const std::string& strArg) const
{
    std::vector<std::string> result = {};
    if (IsArgNegated(strArg)) return result; // special case

    LOCK(cs_args);

    ArgsManagerHelper::AddArgs(result, m_override_args, strArg);
    if (!m_network.empty()) {
        ArgsManagerHelper::AddArgs(result, m_config_args, ArgsManagerHelper::NetworkArg(*this, strArg));
    }

    if (ArgsManagerHelper::UseDefaultSection(*this, strArg)) {
        ArgsManagerHelper::AddArgs(result, m_config_args, strArg);
    }

    return result;
}

bool ArgsManager::IsArgSet(const std::string& strArg) const
{
    if (IsArgNegated(strArg)) return true; // special case
    return ArgsManagerHelper::GetArg(*this, strArg).first;
}

bool ArgsManager::IsArgNegated(const std::string& strArg) const
{
    LOCK(cs_args);

    const auto& ov = m_override_args.find(strArg);
    if (ov != m_override_args.end()) return ov->second.empty();

    if (!m_network.empty()) {
        const auto& cfs = m_config_args.find(ArgsManagerHelper::NetworkArg(*this, strArg));
        if (cfs != m_config_args.end()) return cfs->second.empty();
    }

    const auto& cf = m_config_args.find(strArg);
    if (cf != m_config_args.end()) return cf->second.empty();

    return false;
}

std::string ArgsManager::GetArg(const std::string& strArg, const std::string& strDefault) const
{
    if (IsArgNegated(strArg)) return "0";
    std::pair<bool,std::string> found_res = ArgsManagerHelper::GetArg(*this, strArg);
    if (found_res.first) return found_res.second;
    return strDefault;
}

int64_t ArgsManager::GetArg(const std::string& strArg, int64_t nDefault) const
{
    if (IsArgNegated(strArg)) return 0;
    std::pair<bool,std::string> found_res = ArgsManagerHelper::GetArg(*this, strArg);
    if (found_res.first) return atoi64(found_res.second);
    return nDefault;
}

bool ArgsManager::GetBoolArg(const std::string& strArg, bool fDefault) const
{
    if (IsArgNegated(strArg)) return false;
    std::pair<bool,std::string> found_res = ArgsManagerHelper::GetArg(*this, strArg);
    if (found_res.first) return InterpretBool(found_res.second);
    return fDefault;
}

bool ArgsManager::SoftSetArg(const std::string& strArg, const std::string& strValue)
{
    LOCK(cs_args);
    if (IsArgSet(strArg)) return false;
    ForceSetArg(strArg, strValue);
    return true;
}

bool ArgsManager::SoftSetBoolArg(const std::string& strArg, bool fValue)
{
    if (fValue)
        return SoftSetArg(strArg, std::string("1"));
    else
        return SoftSetArg(strArg, std::string("0"));
}

void ArgsManager::ForceSetArg(const std::string& strArg, const std::string& strValue)
{
    LOCK(cs_args);
    m_override_args[strArg] = {strValue};
}

void ArgsManager::ForceSetArg(const std::string& strArg, int64_t nValue)
{
    ForceSetArg(strArg, i64tostr(nValue));
}

void ArgsManager::AddArg(const std::string& name, const std::string& help, const bool debug_only, const OptionsCategory& cat)
{
    // Split arg name from its help param
    size_t eq_index = name.find('=');
    if (eq_index == std::string::npos) {
        eq_index = name.size();
    }

    std::map<std::string, Arg>& arg_map = m_available_args[cat];
    auto ret = arg_map.emplace(name.substr(0, eq_index), Arg(name.substr(eq_index, name.size() - eq_index), help, debug_only));
    assert(ret.second); // Make sure an insertion actually happened
}

void ArgsManager::AddHiddenArgs(const std::vector<std::string>& names)
{
    for (const std::string& name : names) {
        AddArg(name, "", false, OptionsCategory::HIDDEN);
    }
}

std::string ArgsManager::GetHelpMessage() const
{
    const bool show_debug = gArgs.GetBoolArg("-help-debug", false);

    std::string usage = "";
    for (const auto& arg_map : m_available_args) {
        switch(arg_map.first) {
            case OptionsCategory::OPTIONS:
                usage += HelpMessageGroup("Options:");
                break;
            case OptionsCategory::CONNECTION:
                usage += HelpMessageGroup("Connection options:");
                break;
            case OptionsCategory::ZMQ:
                usage += HelpMessageGroup("ZeroMQ notification options:");
                break;
            case OptionsCategory::DEBUG_TEST:
                usage += HelpMessageGroup("Debugging/Testing options:");
                break;
            case OptionsCategory::NODE_RELAY:
                usage += HelpMessageGroup("Node relay options:");
                break;
            case OptionsCategory::BLOCK_CREATION:
                usage += HelpMessageGroup("Block creation options:");
                break;
            case OptionsCategory::RPC:
                usage += HelpMessageGroup("RPC server options:");
                break;
            case OptionsCategory::WALLET:
                usage += HelpMessageGroup("Wallet options:");
                break;
            case OptionsCategory::WALLET_DEBUG_TEST:
                if (show_debug) usage += HelpMessageGroup("Wallet debugging/testing options:");
                break;
            case OptionsCategory::CHAINPARAMS:
                usage += HelpMessageGroup("Chain selection options:");
                break;
            case OptionsCategory::GUI:
                usage += HelpMessageGroup("UI Options:");
                break;
            case OptionsCategory::COMMANDS:
                usage += HelpMessageGroup("Commands:");
                break;
            case OptionsCategory::REGISTER_COMMANDS:
                usage += HelpMessageGroup("Register Commands:");
                break;
            case OptionsCategory::STATS:
                usage += HelpMessageGroup("Statistic options:");
                break;
            default:
                break;
        }

        // When we get to the hidden options, stop
        if (arg_map.first == OptionsCategory::HIDDEN) break;

        for (const auto& arg : arg_map.second) {
            if (show_debug || !arg.second.m_debug_only) {
                std::string name;
                if (arg.second.m_help_param.empty()) {
                    name = arg.first;
                } else {
                    name = arg.first + arg.second.m_help_param;
                }
                usage += HelpMessageOpt(name, arg.second.m_help_text);
            }
        }
    }
    return usage;
}

bool HelpRequested(const ArgsManager& args)
{
    return args.IsArgSet("-?") || args.IsArgSet("-h") || args.IsArgSet("-help");
}

static const int screenWidth = 79;
static const int optIndent = 2;
static const int msgIndent = 7;

std::string HelpMessageGroup(const std::string &message) {
    return std::string(message) + std::string("\n\n");
}

std::string HelpMessageOpt(const std::string &option, const std::string &message) {
    return std::string(optIndent,' ') + std::string(option) +
           std::string("\n") + std::string(msgIndent,' ') +
           FormatParagraph(message, screenWidth - msgIndent, msgIndent) +
           std::string("\n\n");
}

static std::string FormatException(const std::exception* pex, const char* pszThread)
{
#ifdef WIN32
    char pszModule[MAX_PATH] = "";
    GetModuleFileNameA(nullptr, pszModule, sizeof(pszModule));
#else
    const char* pszModule = "bitcoin";
#endif
    if (pex)
        return strprintf(
            "EXCEPTION: %s       \n%s       \n%s in %s       \n", typeid(*pex).name(), pex->what(), pszModule, pszThread);
    else
        return strprintf(
            "UNKNOWN EXCEPTION       \n%s in %s       \n", pszModule, pszThread);
}

void PrintExceptionContinue(const std::exception* pex, const char* pszThread)
{
    std::string message = FormatException(pex, pszThread);
    LogPrintf("\n\n************************\n%s\n", message);
    fprintf(stderr, "\n\n************************\n%s\n", message.c_str());
}

fs::path GetDefaultDataDir()
{
    // Windows < Vista: C:\Documents and Settings\Username\Application Data\Bitcoin
    // Windows >= Vista: C:\Users\Username\AppData\Roaming\Bitcoin
    // Mac: ~/Library/Application Support/Bitcoin
    // Unix: ~/.bitcoin
#ifdef WIN32
    // Windows
    return GetSpecialFolderPath(CSIDL_APPDATA) / "Bitcoin";
#else
    fs::path pathRet;
    char* pszHome = getenv("HOME");
    if (pszHome == nullptr || strlen(pszHome) == 0)
        pathRet = fs::path("/");
    else
        pathRet = fs::path(pszHome);
#ifdef MAC_OSX
    // Mac
    return pathRet / "Library/Application Support/Bitcoin";
#else
    // Unix
    return pathRet / ".bitcoin";
#endif
#endif
}

static fs::path g_blocks_path_cached;
static fs::path g_blocks_path_cache_net_specific;
static fs::path pathCached;
static fs::path pathCachedNetSpecific;
static CCriticalSection csPathCached;

const fs::path &GetBlocksDir(bool fNetSpecific)
{

    LOCK(csPathCached);

    fs::path &path = fNetSpecific ? g_blocks_path_cache_net_specific : g_blocks_path_cached;

    // This can be called during exceptions by LogPrintf(), so we cache the
    // value so we don't have to do memory allocations after that.
    if (!path.empty())
        return path;

    if (gArgs.IsArgSet("-blocksdir")) {
        path = fs::system_complete(gArgs.GetArg("-blocksdir", ""));
        if (!fs::is_directory(path)) {
            path = "";
            return path;
        }
    } else {
        path = GetDataDir(false);
    }
    if (fNetSpecific)
        path /= BaseParams().DataDir();

    path /= "blocks";
    fs::create_directories(path);
    return path;
}

const fs::path &GetDataDir(bool fNetSpecific)
{

    LOCK(csPathCached);

    fs::path &path = fNetSpecific ? pathCachedNetSpecific : pathCached;

    // This can be called during exceptions by LogPrintf(), so we cache the
    // value so we don't have to do memory allocations after that.
    if (!path.empty())
        return path;

    if (gArgs.IsArgSet("-datadir")) {
        path = fs::system_complete(gArgs.GetArg("-datadir", ""));
        if (!fs::is_directory(path)) {
            path = "";
            return path;
        }
    } else {
        path = GetDefaultDataDir();
    }
    if (fNetSpecific)
        path /= BaseParams().DataDir();

    if (fs::create_directories(path)) {
        // This is the first run, create wallets subdirectory too
        fs::create_directories(path / "wallets");
    }

    return path;
}

void ClearDatadirCache()
{
    LOCK(csPathCached);

    pathCached = fs::path();
    pathCachedNetSpecific = fs::path();
    g_blocks_path_cached = fs::path();
    g_blocks_path_cache_net_specific = fs::path();
}

fs::path GetConfigFile(const std::string& confPath)
{
    return AbsPathForConfigVal(fs::path(confPath), false);
}

fs::path GetRWConfigFile(const std::string& confPath)
{
    return AbsPathForConfigVal(fs::path(confPath));
}

static std::string TrimString(const std::string& str, const std::string& pattern)
{
    std::string::size_type front = str.find_first_not_of(pattern);
    if (front == std::string::npos) {
        return std::string();
    }
    std::string::size_type end = str.find_last_not_of(pattern);
    return str.substr(front, end - front + 1);
}

static bool GetConfigOptions(std::istream& stream, std::string& error, std::vector<std::pair<std::string, std::string>> &options)
{
    std::string str, prefix;
    std::string::size_type pos;
    int linenr = 1;
    while (std::getline(stream, str)) {
        if ((pos = str.find('#')) != std::string::npos) {
            str = str.substr(0, pos);
        }
        const static std::string pattern = " \t\r\n";
        str = TrimString(str, pattern);
        if (!str.empty()) {
            if (*str.begin() == '[' && *str.rbegin() == ']') {
                prefix = str.substr(1, str.size() - 2) + '.';
            } else if (*str.begin() == '-') {
                error = strprintf("parse error on line %i: %s, options in configuration file must be specified without leading -", linenr, str);
                return false;
            } else if ((pos = str.find('=')) != std::string::npos) {
                std::string name = prefix + TrimString(str.substr(0, pos), pattern);
                std::string value = TrimString(str.substr(pos + 1), pattern);
                options.emplace_back(name, value);
            } else {
                error = strprintf("parse error on line %i: %s", linenr, str);
                if (str.size() >= 2 && str.substr(0, 2) == "no") {
                    error += strprintf(", if you intended to specify a negated option, use %s=1 instead", str);
                }
                return false;
            }
        }
        ++linenr;
    }
    return true;
}

bool ArgsManager::ReadConfigStream(std::istream& stream, std::string& error, bool ignore_invalid_keys, bool prepend, bool make_net_specific)
{
    LOCK(cs_args);
    std::vector<std::pair<std::string, std::string>> options;
    if (!GetConfigOptions(stream, error, options)) {
        return false;
    }
    std::map<std::string, size_t> offsets;
    for (const std::pair<std::string, std::string>& option : options) {
        std::string strKey = std::string("-") + option.first;
        std::string strValue = option.second;
        const bool is_negated = InterpretNegatedOption(strKey, strValue);

        if (make_net_specific) {
            strKey = ArgsManagerHelper::NetworkArg(*this, strKey);
        }

        if (is_negated) {
            auto& opt_values = m_config_args[strKey];
            if (prepend) {
                // only clear entries created by this config file
                if (offsets.count(strKey)) {
                    opt_values.erase(opt_values.begin(), opt_values.begin() + offsets.at(strKey));
                    offsets.erase(strKey);
                }
            } else {
                opt_values.clear();
            }
        } else {
            auto& opt_values = m_config_args[strKey];
            if (prepend) {
                opt_values.insert(opt_values.begin() + offsets[strKey], strValue);
                ++offsets[strKey];
            } else {
                opt_values.push_back(strValue);
            }
        }

        // Check that the arg is known
        if (!IsArgKnown(strKey)) {
            if (!ignore_invalid_keys) {
                error = strprintf("Invalid configuration value %s", option.first.c_str());
                return false;
            } else {
                LogPrintf("Ignoring unknown configuration value %s\n", option.first);
            }
        }
    }
    return true;
}

bool ArgsManager::ReadConfigFiles(std::string& error, bool ignore_invalid_keys)
{
    {
        LOCK(cs_args);
        m_config_args.clear();
    }

    const std::string confPath = GetArg("-conf", BITCOIN_CONF_FILENAME);
    fs::ifstream stream(GetConfigFile(confPath));

    // ok to not have a config file
    if (stream.good()) {
        if (!ReadConfigStream(stream, error, ignore_invalid_keys)) {
            return false;
        }
        // if there is an -includeconf in the override args, but it is empty, that means the user
        // passed '-noincludeconf' on the command line, in which case we should not include anything
        if (m_override_args.count("-includeconf") == 0) {
            std::string chain_id = GetChainName();
            std::vector<std::string> includeconf(GetArgs("-includeconf"));
            {
                // We haven't set m_network yet (that happens in SelectParams()), so manually check
                // for network.includeconf args.
                std::vector<std::string> includeconf_net(GetArgs(std::string("-") + chain_id + ".includeconf"));
                includeconf.insert(includeconf.end(), includeconf_net.begin(), includeconf_net.end());
            }

            // Remove -includeconf from configuration, so we can warn about recursion
            // later
            {
                LOCK(cs_args);
                m_config_args.erase("-includeconf");
                m_config_args.erase(std::string("-") + chain_id + ".includeconf");
            }

            for (const std::string& to_include : includeconf) {
                fs::ifstream include_config(GetConfigFile(to_include));
                if (include_config.good()) {
                    if (!ReadConfigStream(include_config, error, ignore_invalid_keys)) {
                        return false;
                    }
                    LogPrintf("Included configuration file %s\n", to_include.c_str());
                } else {
                    error = "Failed to include configuration file " + to_include;
                    return false;
                }
            }

            // Warn about recursive -includeconf
            includeconf = GetArgs("-includeconf");
            {
                std::vector<std::string> includeconf_net(GetArgs(std::string("-") + chain_id + ".includeconf"));
                includeconf.insert(includeconf.end(), includeconf_net.begin(), includeconf_net.end());
                std::string chain_id_final = GetChainName();
                if (chain_id_final != chain_id) {
                    // Also warn about recursive includeconf for the chain that was specified in one of the includeconfs
                    includeconf_net = GetArgs(std::string("-") + chain_id_final + ".includeconf");
                    includeconf.insert(includeconf.end(), includeconf_net.begin(), includeconf_net.end());
                }
            }
            for (const std::string& to_include : includeconf) {
                fprintf(stderr, "warning: -includeconf cannot be used from included files; ignoring -includeconf=%s\n", to_include.c_str());
            }
        }
    }

    // Check for -testnet or -regtest parameter (BaseParams() calls are only valid after this clause)
    try {
        SelectBaseParams(gArgs.GetChainName());
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }

    // If datadir is changed in .conf file:
    ClearDatadirCache();
    if (!fs::is_directory(GetDataDir(false))) {
        error = strprintf("specified data directory \"%s\" does not exist.", gArgs.GetArg("-datadir", "").c_str());
        return false;
    }

    const std::string rwconf_path_str = GetArg("-confrw", BITCOIN_RW_CONF_FILENAME);
    rwconf_path = GetRWConfigFile(rwconf_path_str);
    fs::ifstream rwconf_stream(rwconf_path);
    if (rwconf_stream.good()) {
        // HACK: Save the bitcoin.conf prune setting so we can detect rwconf setting it
        boost::optional<std::vector<std::string>> conf_prune_setting;
        if (m_config_args.count("-prune")) {
            conf_prune_setting = m_config_args.at("-prune");
            m_config_args.erase("-prune");
        }

        // confrw gets prepended before conf settings, and is always network-specific (it's in the network-specific datadir)
        if (!ReadConfigStream(rwconf_stream, error, ignore_invalid_keys, true, true)) {
            return false;
        }

        if (m_config_args.count("-prune")) {
            // HACK: It's only safe to discard conf_prune_setting here because it's not a multi-value option
            rwconf_had_prune_option = true;
        } else if (conf_prune_setting != boost::none) {
            m_config_args["-prune"] = conf_prune_setting.get();
        }
    }
    if (!rwconf_queued_writes.empty()) {
        ModifyRWConfigFile(rwconf_queued_writes);
        rwconf_queued_writes.clear();
    }

    return true;
}

std::string ArgsManager::GetChainName() const
{
    bool fRegTest = ArgsManagerHelper::GetNetBoolArg(*this, "-regtest");
    bool fTestNet = ArgsManagerHelper::GetNetBoolArg(*this, "-testnet");

    if (fTestNet && fRegTest)
        throw std::runtime_error("Invalid combination of -regtest and -testnet.");
    if (fRegTest)
        return CBaseChainParams::REGTEST;
    if (fTestNet)
        return CBaseChainParams::TESTNET;
    return CBaseChainParams::MAIN;
}

namespace {

    // Like std::getline, but includes the EOL character in the result
    bool getline_with_eol(std::istream& stream, std::string& result)
    {
        int current_char;
        current_char = stream.get();
        if (current_char == std::char_traits<char>::eof()) {
            return false;
        }
        result.clear();
        result.push_back(char(current_char));
        while (current_char != '\n') {
            current_char = stream.get();
            if (current_char == std::char_traits<char>::eof()) {
                break;
            }
            result.push_back(char(current_char));
        }
        return true;
    }

    const char * const ModifyRWConfigFile_ws_chars = " \t\r\n";

    void ModifyRWConfigFile_SanityCheck(const std::string& s)
    {
        if (s.empty()) {
            // Dereferencing .begin or .rbegin below is invalid unless the string has at least one character.
            return;
        }

        static const char * const newline_chars = "\r\n";
        static std::string ws_chars(ModifyRWConfigFile_ws_chars);
        if (s.find_first_of(newline_chars) != std::string::npos) {
            throw std::invalid_argument("New-line in config name/value");
        }
        if (ws_chars.find(*s.begin()) != std::string::npos || ws_chars.find(*s.rbegin()) != std::string::npos) {
            throw std::invalid_argument("Config name/value has leading/trailing whitespace");
        }
    }

    void ModifyRWConfigFile_WriteRemaining(std::ostream& stream_out, const std::map<std::string, std::string>& settings_to_change, std::set<std::string>& setFound)
    {
        for (const auto& setting_pair : settings_to_change) {
            const std::string& key = setting_pair.first;
            const std::string& val = setting_pair.second;
            if (setFound.find(key) != setFound.end()) {
                continue;
            }
            setFound.insert(key);
            ModifyRWConfigFile_SanityCheck(key);
            ModifyRWConfigFile_SanityCheck(val);
            stream_out << key << "=" << val << "\n";
        }
    }
} // namespace

void ModifyRWConfigStream(std::istream& stream_in, std::ostream& stream_out, const std::map<std::string, std::string>& settings_to_change)
{
    static const char * const ws_chars = ModifyRWConfigFile_ws_chars;
    std::set<std::string> setFound;
    std::string s, lineend, linebegin, key;
    std::string::size_type n, n2;
    bool inside_group = false, have_eof_nl = true;
    std::map<std::string, std::string>::const_iterator iterCS;
    size_t lineno = 0;
    while (getline_with_eol(stream_in, s)) {
        ++lineno;

        have_eof_nl = (!s.empty()) && (*s.rbegin() == '\n');
        n = s.find('#');
        const bool has_comment = (n != std::string::npos);
        if (!has_comment) {
            n = s.size();
        }
        if (n > 0) {
            n2 = s.find_last_not_of(ws_chars, n - 1);
            if (n2 != std::string::npos) {
                n = n2 + 1;
            }
        }
        n2 = s.find_first_not_of(ws_chars);
        if (n2 == std::string::npos || n2 >= n) {
            // Blank or comment-only line
            stream_out << s;
            continue;
        }
        lineend = s.substr(n);
        linebegin = s.substr(0, n2);
        s = s.substr(n2, n - n2);

        // It is impossible for s to be empty here, due to the blank line check above
        if (*s.begin() == '[' && *s.rbegin() == ']') {
            // We don't use sections, so we could possibly just write out the rest of the file - but we need to check for unparsable lines, so we just set a flag to ignore settings from here on
            ModifyRWConfigFile_WriteRemaining(stream_out, settings_to_change, setFound);
            inside_group = true;
            key.clear();

            stream_out << linebegin << s << lineend;
            continue;
        }

        n = s.find('=');
        if (n == std::string::npos) {
            // Bad line; this causes boost to throw an exception when parsing, so we comment out the entire file
            stream_in.seekg(0, std::ios_base::beg);
            stream_out.seekp(0, std::ios_base::beg);
            if (!(stream_in.good() && stream_out.good())) {
                throw std::ios_base::failure("Failed to rewind (to comment out existing file)");
            }
            // First, write out all the settings we intend to set
            setFound.clear();
            ModifyRWConfigFile_WriteRemaining(stream_out, settings_to_change, setFound);
            // We then define a category to ensure new settings get added before the invalid stuff
            stream_out << "[INVALID]\n";
            // Then, describe the problem in a comment
            stream_out << "# Error parsing line " << lineno << ": " << s << "\n";
            // Finally, dump the rest of the file commented out
            while (getline_with_eol(stream_in, s)) {
                stream_out << "#" << s;
            }
            return;
        }

        if (!inside_group) {
            // We don't support/use groups, so once we're inside key is always null to avoid setting anything
            n2 = s.find_last_not_of(ws_chars, n - 1);
            if (n2 == std::string::npos) {
                n2 = n - 1;
            } else {
                ++n2;
            }
            key = s.substr(0, n2);
        }
        if ((!key.empty()) && (iterCS = settings_to_change.find(key)) != settings_to_change.end() && setFound.find(key) == setFound.end()) {
            // This is the key we want to change
            const std::string& val = iterCS->second;
            setFound.insert(key);
            ModifyRWConfigFile_SanityCheck(val);
            if (has_comment) {
                // Rather than change a commented line, comment it out entirely (the existing comment may relate to the value) and replace it
                stream_out << key << "=" << val << "\n";
                linebegin.insert(linebegin.begin(), '#');
            } else {
                // Just modify the value in-line otherwise
                n2 = s.find_first_not_of(ws_chars, n + 1);
                if (n2 == std::string::npos) {
                    n2 = n + 1;
                }
                s = s.substr(0, n2) + val;
            }
        }
        stream_out << linebegin << s << lineend;
    }
    if (setFound.size() < settings_to_change.size()) {
        if (!have_eof_nl) {
            stream_out << "\n";
        }
        ModifyRWConfigFile_WriteRemaining(stream_out, settings_to_change, setFound);
    }
}

void ArgsManager::ModifyRWConfigFile(const std::map<std::string, std::string>& settings_to_change)
{
    if (rwconf_path.empty()) {
        // Queue the change for after rwconf is loaded
        for (auto& it : settings_to_change) {
            rwconf_queued_writes[it.first] = it.second;
        }
        return;
    }
    fs::path rwconf_new_path = rwconf_path;
    rwconf_new_path += ".new";
    const std::string new_path_str = rwconf_new_path.string();
    try {
        std::remove(new_path_str.c_str());
        fs::ofstream streamRWConfigOut(rwconf_new_path, std::ios_base::out | std::ios_base::trunc);
        if (fs::exists(rwconf_path)) {
            fs::ifstream streamRWConfig(rwconf_path);
            ::ModifyRWConfigStream(streamRWConfig, streamRWConfigOut, settings_to_change);
        } else {
            std::istringstream streamIn;
            ::ModifyRWConfigStream(streamIn, streamRWConfigOut, settings_to_change);
        }
    } catch (...) {
        std::remove(new_path_str.c_str());
        throw;
    }
    if (!RenameOver(rwconf_new_path, rwconf_path)) {
        std::remove(new_path_str.c_str());
        throw std::ios_base::failure(strprintf("Failed to replace %s", new_path_str));
    }
}

void ArgsManager::ModifyRWConfigFile(const std::string& setting_to_change, const std::string& new_value)
{
    std::map<std::string, std::string> settings_to_change;
    settings_to_change[setting_to_change] = new_value;
    ModifyRWConfigFile(settings_to_change);
}

void ArgsManager::EraseRWConfigFile()
{
    assert(!rwconf_path.empty());
    if (!fs::exists(rwconf_path)) {
        return;
    }
    fs::path rwconf_reset_path = rwconf_path;
    rwconf_reset_path += ".reset";
    if (!RenameOver(rwconf_path, rwconf_reset_path)) {
        const std::string path_str = rwconf_path.string();
        if (std::remove(path_str.c_str())) {
            throw std::ios_base::failure(strprintf("Failed to remove %s", path_str));
        }
    }
}

#ifndef WIN32
fs::path GetPidFile()
{
    return AbsPathForConfigVal(fs::path(gArgs.GetArg("-pid", BITCOIN_PID_FILENAME)));
}

void CreatePidFile(const fs::path &path, pid_t pid)
{
    FILE* file = fsbridge::fopen(path, "w");
    if (file)
    {
        fprintf(file, "%d\n", pid);
        fclose(file);
    }
}
#endif

bool RenameOver(fs::path src, fs::path dest)
{
#ifdef WIN32
    return MoveFileExA(src.string().c_str(), dest.string().c_str(),
                       MOVEFILE_REPLACE_EXISTING) != 0;
#else
    int rc = std::rename(src.string().c_str(), dest.string().c_str());
    return (rc == 0);
#endif /* WIN32 */
}

/**
 * Ignores exceptions thrown by Boost's create_directories if the requested directory exists.
 * Specifically handles case where path p exists, but it wasn't possible for the user to
 * write to the parent directory.
 */
bool TryCreateDirectories(const fs::path& p)
{
    try
    {
        return fs::create_directories(p);
    } catch (const fs::filesystem_error&) {
        if (!fs::exists(p) || !fs::is_directory(p))
            throw;
    }

    // create_directories didn't create the directory, it had to have existed already
    return false;
}

bool FileCommit(FILE *file)
{
    if (fflush(file) != 0) { // harmless if redundantly called
        LogPrintf("%s: fflush failed: %d\n", __func__, errno);
        return false;
    }
#ifdef WIN32
    HANDLE hFile = (HANDLE)_get_osfhandle(_fileno(file));
    if (FlushFileBuffers(hFile) == 0) {
        LogPrintf("%s: FlushFileBuffers failed: %d\n", __func__, GetLastError());
        return false;
    }
#elif defined(MAC_OSX) && defined(F_FULLFSYNC)
    if (fcntl(fileno(file), F_FULLFSYNC, 0) == -1) { // Manpage says "value other than -1" is returned on success
        LogPrintf("%s: fcntl F_FULLFSYNC failed: %d\n", __func__, errno);
        return false;
    }
#elif HAVE_FDATASYNC
    if (fdatasync(fileno(file)) != 0 && errno != EINVAL) { // Ignore EINVAL for filesystems that don't support sync
        LogPrintf("%s: fdatasync failed: %d\n", __func__, errno);
        return false;
    }
#else
    if (fsync(fileno(file)) != 0 && errno != EINVAL) {
        LogPrintf("%s: fsync failed: %d\n", __func__, errno);
        return false;
    }
#endif
    return true;
}

void DirectoryCommit(const fs::path &dirname)
{
#ifndef WIN32
    FILE* file = fsbridge::fopen(dirname, "r");
    if (file) {
        fsync(fileno(file));
        fclose(file);
    }
#endif
}

bool TruncateFile(FILE *file, unsigned int length) {
#if defined(WIN32)
    return _chsize(_fileno(file), length) == 0;
#else
    return ftruncate(fileno(file), length) == 0;
#endif
}

/**
 * this function tries to raise the file descriptor limit to the requested number.
 * It returns the actual file descriptor limit (which may be more or less than nMinFD)
 */
int RaiseFileDescriptorLimit(int nMinFD) {
#if defined(WIN32)
    return 2048;
#else
    struct rlimit limitFD;
    if (getrlimit(RLIMIT_NOFILE, &limitFD) != -1) {
        if (limitFD.rlim_cur < (rlim_t)nMinFD) {
            limitFD.rlim_cur = nMinFD;
            if (limitFD.rlim_cur > limitFD.rlim_max)
                limitFD.rlim_cur = limitFD.rlim_max;
            setrlimit(RLIMIT_NOFILE, &limitFD);
            getrlimit(RLIMIT_NOFILE, &limitFD);
        }
        return limitFD.rlim_cur;
    }
    return nMinFD; // getrlimit failed, assume it's fine
#endif
}

/**
 * this function tries to make a particular range of a file allocated (corresponding to disk space)
 * it is advisory, and the range specified in the arguments will never contain live data
 */
void AllocateFileRange(FILE *file, unsigned int offset, unsigned int length) {
#if defined(WIN32)
    // Windows-specific version
    HANDLE hFile = (HANDLE)_get_osfhandle(_fileno(file));
    LARGE_INTEGER nFileSize;
    int64_t nEndPos = (int64_t)offset + length;
    nFileSize.u.LowPart = nEndPos & 0xFFFFFFFF;
    nFileSize.u.HighPart = nEndPos >> 32;
    SetFilePointerEx(hFile, nFileSize, 0, FILE_BEGIN);
    SetEndOfFile(hFile);
#elif defined(MAC_OSX)
    // OSX specific version
    fstore_t fst;
    fst.fst_flags = F_ALLOCATECONTIG;
    fst.fst_posmode = F_PEOFPOSMODE;
    fst.fst_offset = 0;
    fst.fst_length = (off_t)offset + length;
    fst.fst_bytesalloc = 0;
    if (fcntl(fileno(file), F_PREALLOCATE, &fst) == -1) {
        fst.fst_flags = F_ALLOCATEALL;
        fcntl(fileno(file), F_PREALLOCATE, &fst);
    }
    ftruncate(fileno(file), fst.fst_length);
#elif _POSIX_C_SOURCE >= 200112L
    // Use posix_fallocate to advise the kernel how much data we have to write,
    // if this system supports it.
    off_t nEndPos = (off_t)offset + length;
    posix_fallocate(fileno(file), 0, nEndPos);
#else
    // Fallback version
    // TODO: just write one byte per block
    static const char buf[65536] = {};
    if (fseek(file, offset, SEEK_SET)) {
        return;
    }
    while (length > 0) {
        unsigned int now = 65536;
        if (length < now)
            now = length;
        fwrite(buf, 1, now, file); // allowed to fail; this function is advisory anyway
        length -= now;
    }
#endif
}

FILE* AdviseSequential(FILE *file) {
#if _POSIX_C_SOURCE >= 200112L
    // Since this whole thing is advisory anyway, we can ignore any errors
    // encountered up to and including the posix_fadvise call. However, we must
    // rewind the file to the appropriate position if we've changed the seek
    // offset.
    if (file == nullptr)
        return nullptr;
    int fd = fileno(file);
    if (fd == -1)
        return file;
    off_t start = lseek(fd, 0, SEEK_CUR);
    if (start == -1)
        return file;
    off_t end = lseek(fd, 0, SEEK_END);
    if (end != -1) {
        posix_fadvise(fd, start, end - start, POSIX_FADV_WILLNEED);
        posix_fadvise(fd, start, end - start, POSIX_FADV_SEQUENTIAL);
    }
    lseek(fd, start, SEEK_SET);
#endif
    return file;
}

int CloseAndDiscard(FILE *file) {
#if _POSIX_C_SOURCE >= 200112L
    // Ignore any errors up to and including the posix_fadvise call since it's
    // advisory.
    if (file != nullptr) {
        off_t end;
        int fd = fileno(file);
        if (fd == -1)
            goto close;
        end = lseek(fd, 0, SEEK_END);
        if (end == -1)
            goto close;
        posix_fadvise(fd, 0, end, POSIX_FADV_DONTNEED);
    }
#endif
 close:
    return fclose(file);
}

#ifdef WIN32
fs::path GetSpecialFolderPath(int nFolder, bool fCreate)
{
    char pszPath[MAX_PATH] = "";

    if(SHGetSpecialFolderPathA(nullptr, pszPath, nFolder, fCreate))
    {
        return fs::path(pszPath);
    }

    LogPrintf("SHGetSpecialFolderPathA() failed, could not obtain requested path.\n");
    return fs::path("");
}
#endif

void runCommand(const std::string& strCommand)
{
    if (strCommand.empty()) return;
    int nErr = ::system(strCommand.c_str());
    if (nErr)
        LogPrintf("runCommand error: system(%s) returned %d\n", strCommand, nErr);
}

void RenameThread(const char* name)
{
#if defined(PR_SET_NAME)
    // Only the first 15 characters are used (16 - NUL terminator)
    ::prctl(PR_SET_NAME, name, 0, 0, 0);
#elif (defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__DragonFly__))
    pthread_set_name_np(pthread_self(), name);

#elif defined(MAC_OSX)
    pthread_setname_np(name);
#else
    // Prevent warnings for unused parameters...
    (void)name;
#endif
}

void SetupEnvironment()
{
#ifdef HAVE_MALLOPT_ARENA_MAX
    // glibc-specific: On 32-bit systems set the number of arenas to 1.
    // By default, since glibc 2.10, the C library will create up to two heap
    // arenas per core. This is known to cause excessive virtual address space
    // usage in our usage. Work around it by setting the maximum number of
    // arenas to 1.
    if (sizeof(void*) == 4) {
        mallopt(M_ARENA_MAX, 1);
    }
#endif
    // On most POSIX systems (e.g. Linux, but not BSD) the environment's locale
    // may be invalid, in which case the "C" locale is used as fallback.
#if !defined(WIN32) && !defined(MAC_OSX) && !defined(__FreeBSD__) && !defined(__OpenBSD__)
    try {
        std::locale(""); // Raises a runtime error if current locale is invalid
    } catch (const std::runtime_error&) {
        setenv("LC_ALL", "C", 1);
    }
#endif
    // The path locale is lazy initialized and to avoid deinitialization errors
    // in multithreading environments, it is set explicitly by the main thread.
    // A dummy locale is used to extract the internal default locale, used by
    // fs::path, which is then used to explicitly imbue the path.
    std::locale loc = fs::path::imbue(std::locale::classic());
    fs::path::imbue(loc);
}

bool SetupNetworking()
{
#ifdef WIN32
    // Initialize Windows Sockets
    WSADATA wsadata;
    int ret = WSAStartup(MAKEWORD(2,2), &wsadata);
    if (ret != NO_ERROR || LOBYTE(wsadata.wVersion ) != 2 || HIBYTE(wsadata.wVersion) != 2)
        return false;
#endif
    return true;
}

int GetNumCores()
{
    return std::thread::hardware_concurrency();
}

std::string CopyrightHolders(const std::string& strPrefix)
{
    std::string strCopyrightHolders = strPrefix + strprintf(_(COPYRIGHT_HOLDERS), _(COPYRIGHT_HOLDERS_SUBSTITUTION));

    // Check for untranslated substitution to make sure Bitcoin Core copyright is not removed by accident
    if (strprintf(COPYRIGHT_HOLDERS, COPYRIGHT_HOLDERS_SUBSTITUTION).find("Bitcoin Core") == std::string::npos) {
        strCopyrightHolders += "\n" + strPrefix + "The Bitcoin Core developers";
    }
    return strCopyrightHolders;
}

// Obtain the application startup time (used for uptime calculation)
int64_t GetStartupTime()
{
    return nStartupTime;
}

fs::path AbsPathForConfigVal(const fs::path& path, bool net_specific)
{
    return fs::absolute(path, GetDataDir(net_specific));
}

int ScheduleBatchPriority(void)
{
#ifdef SCHED_BATCH
    const static sched_param param{0};
    if (int ret = pthread_setschedparam(pthread_self(), SCHED_BATCH, &param)) {
        LogPrintf("Failed to pthread_setschedparam: %s\n", strerror(errno));
        return ret;
    }
    return 0;
#else
    return 1;
#endif
}
