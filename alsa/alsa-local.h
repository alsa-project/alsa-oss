#include "alsa-oss-emul.h"

#if 1
#define DEBUG_POLL
#define DEBUG_SELECT
#ifdef NEW_MACRO_VARARGS
#define DEBUG(...) do { if (alsa_oss_debug) fprintf(stderr, __VA_ARGS__); } while (0)
#else /* !NEW_MACRO_VARARGS */
#define DEBUG(args...) do { if (alsa_oss_debug) fprintf(stderr, ##args); } while (0)
#endif
#else
#ifdef NEW_MACRO_VARARGS
#define DEBUG(...)
#else /* !NEW_MACRO_VARARGS */
#define DEBUG(args...)
#endif
#endif

extern int alsa_oss_debug;
extern snd_output_t *alsa_oss_debug_out;
