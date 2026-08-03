#ifndef GHOSTESP_SHELL_H
#define GHOSTESP_SHELL_H

#include <stdbool.h>
#include <stddef.h>

/* Expand one user command before the normal CLI tokenizer runs. */
bool shell_expand_command(const char *input, char *output, size_t output_len);

/* Prompt and boot configuration used by the serial console and main banner. */
void shell_get_prompt(char *output, size_t output_len);
void shell_set_hostname(const char *hostname);
bool shell_get_banner_enabled(void);
void shell_set_banner_enabled(bool enabled);
void shell_set_color(const char *color);
const char *shell_get_color(void);
bool shell_stop_watch(void);

/* Print a close command match for an unknown command. */
void shell_suggest_command(const char *command);
void shell_print_command_help(const char *command);

/* Headless shell command handlers. */
void handle_echo_cmd(int argc, char **argv);
void handle_ifconfig_cmd(int argc, char **argv);
void handle_ping_cmd(int argc, char **argv);
void handle_version_cmd(int argc, char **argv);
void handle_uuid_cmd(int argc, char **argv);
void handle_macaddr_cmd(int argc, char **argv);
void handle_uptime_cmd(int argc, char **argv);
void handle_whoami_cmd(int argc, char **argv);
void handle_status_cmd(int argc, char **argv);
void handle_clear_cmd(int argc, char **argv);
void handle_hostname_cmd(int argc, char **argv);
void handle_color_cmd(int argc, char **argv);
void handle_banner_cmd(int argc, char **argv);
void handle_alias_cmd(int argc, char **argv);
void handle_unalias_cmd(int argc, char **argv);
void handle_history_cmd(int argc, char **argv);
void handle_didyoumean_cmd(int argc, char **argv);
void handle_ps_cmd(int argc, char **argv);
void handle_df_cmd(int argc, char **argv);
void handle_tail_cmd(int argc, char **argv);
void handle_grep_cmd(int argc, char **argv);
void handle_source_cmd(int argc, char **argv);
void handle_tee_cmd(int argc, char **argv);
void handle_env_cmd(int argc, char **argv);
void handle_export_cmd(int argc, char **argv);
void handle_watch_cmd(int argc, char **argv);

#endif
