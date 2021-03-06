#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "logger.h"

LogLevel gLogLevel = LOG_LEVEL_INFO;
static char* mLogFilePath = "grvk.log";
static FILE* mLogFile = NULL;

static void pickLogLevel()
{
    const char* levelNames[] = { "trace", "verbose", "debug", "info", "warning", "error", "none" };
    char* envValue = getenv("GRVK_LOG_LEVEL");

    if (envValue != NULL) {
        for (int i = LOG_LEVEL_TRACE; i <= LOG_LEVEL_NONE; i++) {
            if (strcmp(envValue, levelNames[i]) == 0) {
                gLogLevel = i;
                break;
            }
        }
    }
}

static void pickLogFile()
{
    char* envValue = getenv("GRVK_LOG_PATH");

    if (envValue != NULL) {
        if (strlen(envValue) == 0) {
            mLogFilePath = NULL;
        } else {
            mLogFilePath = envValue;
        }
    }
}

void logInit()
{
    pickLogLevel();
    pickLogFile();

    if (gLogLevel != LOG_LEVEL_NONE && mLogFilePath != NULL) {
        mLogFile = fopen(mLogFilePath, "w");
    }
}

void logPrint(
    LogLevel level,
    const char* name,
    const char* format,
    ...)
{
    const char* prefixes[] = { "T", "V", "D", "I", "W", "E", "" };
    fprintf(stdout, "%s/%s: ", prefixes[level], name);
    if (mLogFile != NULL) {
        fprintf(mLogFile, "%s/%s: ", prefixes[level], name);
    }

    va_list argptr;
    va_start(argptr, format);
    vfprintf(stdout, format, argptr);
    if (mLogFile != NULL) {
        vfprintf(mLogFile, format, argptr);
        fflush(mLogFile);
    }
    va_end(argptr);
}

void logPrintRaw(
    const char* format,
    ...)
{
    if (gLogLevel == LOG_LEVEL_NONE) {
        return;
    }

    va_list argptr;
    va_start(argptr, format);
    vfprintf(stdout, format, argptr);
    if (mLogFile != NULL) {
        vfprintf(mLogFile, format, argptr);
        fflush(mLogFile);
    }
    va_end(argptr);
}

