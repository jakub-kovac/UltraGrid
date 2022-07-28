/*
 * FILE:    debug.cpp
 * PROGRAM: RAT
 * AUTHORS: Isidor Kouvelas
 *          Colin Perkins
 *          Mark Handley
 *          Orion Hodson
 *          Jerry Isdale
 *
 * $Revision: 1.1 $
 * $Date: 2007/11/08 09:48:59 $
 *
 * Copyright (c) 1995-2000 University College London
 * Copyright (c) 2005-2021 CESNET, z. s. p. o.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, is permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *      This product includes software developed by the Computer Science
 *      Department at University College London
 * 4. Neither the name of the University nor of the Department may be used
 *    to endorse or promote products derived from this software without
 *    specific prior written permission.
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESSED OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif // defined HAVE_CONFIG_H
#include "config_unix.h"
#include "config_win32.h"

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>

#include "compat/misc.h" // strdupa
#include "compat/platform_time.h"
#include "debug.h"
#include "host.h"
#include "rang.hpp"
#include "utils/color_out.h"
#include "utils/misc.h" // ug_strerror

using std::string;
using std::unordered_map;

volatile int log_level = LOG_LEVEL_INFO;

static void _dprintf(const char *format, ...)
{
        if (log_level < LOG_LEVEL_DEBUG) {
                return;
        }

#ifdef WIN32
        char msg[65535];
        va_list ap;

        va_start(ap, format);
        _vsnprintf(msg, 65535, format, ap);
        va_end(ap);
        OutputDebugString(msg);
#else
        va_list ap;

        va_start(ap, format);
        vfprintf(stderr, format, ap);
        va_end(ap);
#endif                          /* WIN32 */
}

void log_msg(int level, const char *format, ...)
{
        va_list ap;

        if (log_level < level) {
                return;
        }

#if 0 // WIN32
        if (log_level == LOG_LEVEL_DEBUG) {
                char msg[65535];
                va_list ap;

                va_start(ap, format);
                _vsnprintf(msg, 65535, format, ap);
                va_end(ap);
                OutputDebugString(msg);
                return;
        }
#endif                          /* WIN32 */

        // get number of required bytes
        va_start(ap, format);
        int size = vsnprintf(NULL, 0, format, ap);
        va_end(ap);

        // format the string
        char *buffer = (char *) alloca(size + 1);
        va_start(ap, format);
        if (vsprintf(buffer, format, ap) != size) {
                va_end(ap);
                return;
        }
        va_end(ap);

        LOG(level) << buffer;
}

void log_msg_once(int level, uint32_t id, const char *msg) {
        if (log_level < level) {
                return;
        }
        Logger(level).once(id, msg);
}

/**
 * This function is analogous to perror(). The message is printed using the logger.
 */
void log_perror(int level, const char *msg)
{
        log_msg(level, "%s: %s\n", msg, ug_strerror(errno));
}

/**
 * debug_dump:
 * @lp: pointer to memory region.
 * @len: length of memory region in bytes.
 *
 * Writes a dump of a memory region to stdout.  The dump contains a
 * hexadecimal and an ascii representation of the memory region.
 *
 **/
void debug_dump(void *lp, int len)
{
        char *p;
        int i, j, start;
        char Buff[81];
        char stuffBuff[10];
        char tmpBuf[10];

        _dprintf("Dump of %d=%x bytes\n", len, len);
        start = 0L;
        while (start < len) {
                /* start line with pointer position key */
                p = (char *)lp + start;
                sprintf(Buff, "%p: ", p);

                /* display each character as hex value */
                for (i = start, j = 0; j < 16; p++, i++, j++) {
                        if (i < len) {
                                sprintf(tmpBuf, "%X", ((int)(*p) & 0xFF));

                                if (strlen((char *)tmpBuf) < 2) {
                                        stuffBuff[0] = '0';
                                        stuffBuff[1] = tmpBuf[0];
                                        stuffBuff[2] = ' ';
                                        stuffBuff[3] = '\0';
                                } else {
                                        stuffBuff[0] = tmpBuf[0];
                                        stuffBuff[1] = tmpBuf[1];
                                        stuffBuff[2] = ' ';
                                        stuffBuff[3] = '\0';
                                }
                                strcat(Buff, stuffBuff);
                        } else
                                strcat(Buff, " ");
                        if (j == 7)     /* space between groups of 8 */
                                strcat(Buff, " ");
                }

                strcat(Buff, "  ");

                /* display each character as character value */
                for (i = start, j = 0, p = (char *)lp + start;
                     (i < len && j < 16); p++, i++, j++) {
                        if (((*p) >= ' ') && ((*p) <= '~'))     /* test displayable */
                                sprintf(tmpBuf, "%c", *p);
                        else
                                sprintf(tmpBuf, "%c", '.');
                        strcat(Buff, tmpBuf);
                        if (j == 7)     /* space between groups of 8 */
                                strcat(Buff, " ");
                }
                _dprintf("%s\n", Buff);
                start = i;      /* next line starting byte */
        }
}

bool set_log_level(const char *optarg,
                bool *logger_repeat_msgs,
                log_timestamp_mode *show_timestamps)
{
        assert(optarg != nullptr);
        assert(logger_repeat_msgs != nullptr);
        assert(show_timestamps != nullptr);

        using namespace std::string_literals;
        using std::clog;
        using std::cout;

        static const struct { const char *name; int level; } mapping[] = {
                { "quiet", LOG_LEVEL_QUIET },
                { "fatal", LOG_LEVEL_FATAL },
                { "error", LOG_LEVEL_ERROR },
                { "warning", LOG_LEVEL_WARNING},
                { "notice", LOG_LEVEL_NOTICE},
                { "info", LOG_LEVEL_INFO  },
                { "verbose", LOG_LEVEL_VERBOSE},
                { "debug", LOG_LEVEL_DEBUG },
                { "debug2", LOG_LEVEL_DEBUG2 },
        };

        if ("help"s == optarg) {
                cout << "log level: [0-" << LOG_LEVEL_MAX;
                for (auto m : mapping) {
                        cout << "|" << m.name;
                }
                cout << "][+repeat][+/-timestamps]\n";
                cout << BOLD("\trepeat") << " - print repeating log messages\n";
                cout << BOLD("\ttimestamps") << " - enable/disable timestamps\n";
                return false;
        }

        if (strstr(optarg, "+repeat") != nullptr) {
                *logger_repeat_msgs = true;
        }

        if (const char *timestamps = strstr(optarg, "timestamps")) {
                if (timestamps > optarg) {
                        if(timestamps[-1] == '+')
                                *show_timestamps = LOG_TIMESTAMP_ENABLED;
                        else
                                *show_timestamps = LOG_TIMESTAMP_DISABLED;
                }
        }

        if (getenv("ULTRAGRID_VERBOSE") != nullptr) {
                log_level = LOG_LEVEL_VERBOSE;
        }

        if (optarg[0] == '+' || optarg[0] == '-') { // only flags, no log level
                return true;
        }

        if (isdigit(optarg[0])) {
                long val = strtol(optarg, nullptr, 0);
                if (val < 0 || val > LOG_LEVEL_MAX) {
                        clog << "Log: wrong value: " << optarg << " (allowed range [0.." << LOG_LEVEL_MAX << "])\n";
                        return false;
                }
                log_level = val;
                return true;
        }

        char *log_level_str = strdupa(optarg);
        if (char *delim = strpbrk(log_level_str, "+-")) {
                *delim = '\0';
        }
        for (auto m : mapping) {
                if (strcmp(log_level_str, m.name) == 0) {
                        log_level = m.level;
                        return true;
                }
        }

        LOG(LOG_LEVEL_ERROR) << "Wrong log level specification: " << optarg << "\n";
        return false;
}

void Logger::preinit(bool skip_repeated, log_timestamp_mode show_timestamps)
{
        Logger::skip_repeated = skip_repeated;
        Logger::show_timestamps = show_timestamps;
        if (rang::rang_implementation::supportsColor()
                        && rang::rang_implementation::isTerminal(std::cout.rdbuf())
                        && rang::rang_implementation::isTerminal(std::cerr.rdbuf())) {
                // force ANSI sequences even when written to ostringstream
                rang::setControlMode(rang::control::Force);
#ifdef _WIN32
                // ANSI control sequences need to be explicitly set in Windows
                if ((rang::rang_implementation::setWinTermAnsiColors(std::cout.rdbuf()) || rang::rang_implementation::isMsysPty(_fileno(stdout))) &&
                                (rang::rang_implementation::setWinTermAnsiColors(std::cerr.rdbuf()) || rang::rang_implementation::isMsysPty(_fileno(stderr)))) {
                        rang::setWinTermMode(rang::winTerm::Ansi);
                }
#endif
        }
}

#ifdef DEBUG
void debug_file_dump(const char *key, void (*serialize)(const void *data, FILE *), void *data) {
        const char *dump_file_val = get_commandline_param("debug-dump");
        static thread_local unordered_map<string, int> skip_map;
        if (dump_file_val == nullptr) {
                return;
        }

        // check if key is contained
        string not_first = ",";
        not_first + key;
        int skip_n = 0;
        if (strstr(dump_file_val, key) == dump_file_val) {
                const char *val = dump_file_val + strlen(key);
                if (val[0] == '=') {
                        skip_n = atoi(val + 1);
                }
        } else if (strstr(dump_file_val, not_first.c_str()) != NULL) {
                const char *val = strstr(dump_file_val, not_first.c_str()) + strlen(key);
                if (val[0] == '=') {
                        skip_n = atoi(val + 1);
                }
        } else {
                return;
        }

        if (skip_map.find(key) == skip_map.end()) {
                skip_map[key] = skip_n;
        }

        if (skip_map[key] == -1) { // already exported
                return;
        }

        if (skip_map[key] > 0) {
                skip_map[key]--;
                return;
        }

        // export
        string name = string(key) + ".dump";
        FILE *out = fopen(name.c_str(), "wb");
        if (out == nullptr) {
                perror("debug_file_dump fopen");
                return;
        }
        serialize(data, out);
        fclose(out);

        skip_map[key] = -1;
}
#endif

std::atomic<Logger::last_message *> Logger::last_msg{};
thread_local std::set<uint32_t> Logger::oneshot_messages;
std::atomic<bool> Logger::skip_repeated{true};
log_timestamp_mode Logger::show_timestamps = LOG_TIMESTAMP_AUTO;

