#pragma once

#include <SDL3/SDL_log.h>

#define pr_log_info(...)  SDL_Log(__VA_ARGS__)
#define pr_log_error(...) SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, __VA_ARGS__)
