#ifndef FOUR_YUE_RUNTIME_H
#define FOUR_YUE_RUNTIME_H

#include <stdint.h>

// Shared-library builds export the stable C ABI on Windows.
#if defined(_WIN32) && defined(FOUR_YUE_RUNTIME_SHARED)
#define FOUR_YUE_RUNTIME_API __declspec(dllexport)
#else
#define FOUR_YUE_RUNTIME_API
#endif

FOUR_YUE_RUNTIME_API int32_t __4yue_read_key(void);
FOUR_YUE_RUNTIME_API int32_t __4yue_sleep_ms(int32_t milliseconds);
FOUR_YUE_RUNTIME_API int32_t __4yue_clear_screen(void);
FOUR_YUE_RUNTIME_API int32_t __4yue_random(int32_t maximum);
FOUR_YUE_RUNTIME_API void __4yue_init_args(int32_t argc, char **argv);
FOUR_YUE_RUNTIME_API int32_t __4yue_argc(void);
FOUR_YUE_RUNTIME_API const char *__4yue_arg(int32_t index);
FOUR_YUE_RUNTIME_API const char *__4yue_env_var(const char *name);
FOUR_YUE_RUNTIME_API int32_t __4yue_envc(void);
FOUR_YUE_RUNTIME_API const char *__4yue_env(int32_t index);

#endif
