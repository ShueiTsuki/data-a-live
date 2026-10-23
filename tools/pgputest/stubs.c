/*
 * Link-time stand-ins for the iOS libraries pgputest uses (no SDK here): built twice by
 * build.sh, as /usr/lib/libSystem.B.dylib and as IOKit's install name, each exporting its
 * part. Only the names matter; on the phone dyld binds to the real libraries.
 */
#ifdef STUB_LIBSYSTEM
unsigned int mach_task_self_;
void close(void) {}
void open(void) {}
void sleep(void) {}
void vsnprintf(void) {}
void write(void) {}
void memset(void) {}
void memcpy(void) {}
void __bzero(void) {}
void stub_binder(void) __asm__("dyld_stub_binder");   /* no leading underscore */
void stub_binder(void) {}
#else
void IOConnectCallStructMethod(void) {}
void IOConnectMapMemory64(void) {}
void IOConnectUnmapMemory64(void) {}
void IOServiceClose(void) {}
void IOServiceGetMatchingService(void) {}
void IOServiceMatching(void) {}
void IOServiceOpen(void) {}
#endif
