#ifndef HOST_LOG_H
#define HOST_LOG_H

/* Messages of the host's own, on stderr; Host_Fatal ends the program with a core dump */
__attribute__((format(printf, 1, 2))) void Host_Log(const char *format, ...);
__attribute__((noreturn, format(printf, 1, 2))) void Host_Fatal(const char *format, ...);

#endif /* HOST_LOG_H */
