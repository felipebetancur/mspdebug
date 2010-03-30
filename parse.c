/* MSPDebug - debugging tool for MSP430 MCUs
 * Copyright (C) 2009, 2010 Daniel Beer
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifdef USE_READLINE
#include <readline/readline.h>
#include <readline/history.h>
#endif

#include "stab.h"
#include "parse.h"

static struct option *option_list;

void register_option(struct option *o)
{
	o->next = option_list;
	option_list = o;
}

static struct option *find_option(const char *name)
{
	struct option *o;

	for (o = option_list; o; o = o->next)
		if (!strcasecmp(o->name, name))
			return o;

	return NULL;
}

static int interactive_call;

int is_interactive(void)
{
	return interactive_call;
}

char *get_arg(char **text)
{
	char *start;
	char *end;

	if (!text)
		return NULL;

	start = *text;
	while (*start && isspace(*start))
		start++;

	if (!*start)
		return NULL;

	end = start;
	while (*end && !isspace(*end))
		end++;

	if (*end)
	    while (*end && isspace(*end))
		    *(end++) = 0;

	*text = end;
	return start;
}

const struct command *find_command(const char *name)
{
	int i;

	for (i = 0; all_commands[i].name; i++)
		if (!strcasecmp(name, all_commands[i].name))
			return &all_commands[i];

	return NULL;
}

int process_command(char *arg, int interactive)
{
	const char *cmd_text;
	int len = strlen(arg);

	while (len && isspace(arg[len - 1]))
		len--;
	arg[len] = 0;

	cmd_text = get_arg(&arg);
	if (cmd_text) {
		const struct command *cmd = find_command(cmd_text);

		if (cmd) {
			int old = interactive_call;
			int ret;

			interactive_call = interactive;
			ret = cmd->func(&arg);
			interactive_call = old;
			return 0;
		}

		fprintf(stderr, "unknown command: %s (try \"help\")\n",
			cmd_text);
		return -1;
	}

	return 0;
}

#ifndef USE_READLINE
#define LINE_BUF_SIZE 128

static char *readline(const char *prompt)
{
	char *buf = malloc(LINE_BUF_SIZE);

	if (!buf) {
		perror("readline: can't allocate memory");
		return NULL;
	}

	for (;;) {
		printf("(mspdebug) ");
		fflush(stdout);

		if (fgets(buf, LINE_BUF_SIZE, stdin))
			return buf;

		if (feof(stdin))
			break;

		printf("\n");
	}

	free(buf);
	return NULL;
}

#define add_history(x)
#endif

void reader_loop(void)
{
	printf("\n");
	cmd_help(NULL);

	for (;;) {
		char *buf = readline("(mspdebug) ");

		if (!buf)
			break;

		add_history(buf);
		process_command(buf, 1);
		free(buf);
	}

	printf("\n");
}

const char *type_text(option_type_t type)
{
	switch (type) {
	case OPTION_BOOLEAN:
		return "boolean";

	case OPTION_NUMERIC:
		return "numeric";

	case OPTION_TEXT:
		return "text";
	}

	return "unknown";
}

int cmd_help(char **arg)
{
	char *topic = get_arg(arg);

	if (topic) {
		const struct command *cmd = find_command(topic);
		const struct option *opt = find_option(topic);

		if (cmd) {
			printf("COMMAND: %s\n", cmd->name);
			fputs(cmd->help, stdout);
			if (opt)
				printf("\n");
		}

		if (opt) {
			printf("OPTION: %s (%s)\n", opt->name,
			       type_text(opt->type));
			fputs(opt->help, stdout);
		}

		if (!(cmd || opt)) {
			fprintf(stderr, "help: unknown command: %s\n", topic);
			return -1;
		}
	} else {
		int i;
		int max_len = 0;
		int rows, cols;
		int total = 0;

		for (i = 0; all_commands[i].name; i++) {
			int len = strlen(all_commands[i].name);

			if (len > max_len)
				max_len = len;
			total++;
		}

		max_len += 2;
		cols = 72 / max_len;
		rows = (total + cols - 1) / cols;

		printf("Available commands:\n");
		for (i = 0; i < rows; i++) {
			int j;

			printf("    ");
			for (j = 0; j < cols; j++) {
				int k = j * rows + i;
				const struct command *cmd = &all_commands[k];

				if (k >= total)
					break;

				printf("%s", cmd->name);
				for (k = strlen(cmd->name); k < max_len; k++)
					printf(" ");
			}

			printf("\n");
		}

		printf("Type \"help <command>\" for more information.\n");
		printf("Press Ctrl+D to quit.\n");
	}

	return 0;
}

static char token_buf[64];
static int token_len;
static int token_mult;
static int token_sum;

static int token_add(void)
{
	int i;
	u_int16_t value;

	if (!token_len)
		return 0;

	token_buf[token_len] = 0;
	token_len = 0;

	/* Is it a decimal? */
	i = 0;
	while (token_buf[i] && isdigit(token_buf[i]))
		i++;
	if (!token_buf[i]) {
		token_sum += token_mult * atoi(token_buf);
		return 0;
	}

	/* Is it hex? */
	if (token_buf[0] == '0' && tolower(token_buf[1]) == 'x') {
		token_sum += token_mult * strtol(token_buf + 2, NULL, 16);
		return 0;
	}

	/* Look up the name in the symbol table */
	if (!stab_get(token_buf, &value)) {
		token_sum += token_mult * value;
		return 0;
	}

	fprintf(stderr, "unknown token: %s\n", token_buf);
	return -1;
}

int addr_exp(const char *text, int *addr)
{
	token_len = 0;
	token_mult = 1;
	token_sum = 0;

	while (*text) {
		if (isalnum(*text) || *text == '_' || *text == '$' ||
		    *text == '.' || *text == ':') {
			if (token_len + 1 < sizeof(token_buf))
				token_buf[token_len++] = *text;
		} else {
			if (token_add() < 0)
				return -1;
			if (*text == '+')
				token_mult = 1;
			if (*text == '-')
				token_mult = -1;
		}

		text++;
	}

	if (token_add() < 0)
		return -1;

	*addr = token_sum & 0xffff;
	return 0;
}

static void display_option(const struct option *o)
{
	printf("%32s = ", o->name);

	switch (o->type) {
	case OPTION_BOOLEAN:
		printf("%s", o->data.numeric ? "true" : "false");
		break;

	case OPTION_NUMERIC:
		printf("0x%x (%d)", o->data.numeric,
		       o->data.numeric);
		break;

	case OPTION_TEXT:
		printf("%s", o->data.text);
		break;
	}

	printf("\n");
}

static int parse_option(struct option *o, const char *word)
{
	switch (o->type) {
	case OPTION_BOOLEAN:
		o->data.numeric = (isdigit(word[0]) && word[0] > '0') ||
			word[0] == 't' || word[0] == 'y' ||
			(word[0] == 'o' && word[1] == 'n');
		break;

	case OPTION_NUMERIC:
		return addr_exp(word, &o->data.numeric);

	case OPTION_TEXT:
		strncpy(o->data.text, word, sizeof(o->data.text));
		o->data.text[sizeof(o->data.text) - 1] = 0;
		break;
	}

	return 0;
}

int cmd_opt(char **arg)
{
	const char *opt_text = get_arg(arg);
	struct option *opt = NULL;

	if (opt_text) {
		opt = find_option(opt_text);
		if (!opt) {
			fprintf(stderr, "opt: no such option: %s\n",
				opt_text);
			return -1;
		}
	}

	if (**arg) {
		if (parse_option(opt, *arg) < 0) {
			fprintf(stderr, "opt: can't parse option: %s\n",
				*arg);
			return -1;
		}
	} else if (opt_text) {
		display_option(opt);
	} else {
		struct option *o;

		for (o = option_list; o; o = o->next)
			display_option(o);
	}

	return 0;
}

static struct option option_color = {
	.name = "color",
	.type = OPTION_BOOLEAN,
	.help = "Colorize disassembly output.\n"
};

int colorize(const char *text)
{
	if (!option_color.data.numeric)
		return 0;

	return printf("\x1b[%s", text);
}

void parse_init(void)
{
	register_option(&option_color);
}