/* vi: set sw=4 ts=4: */
/*
 * sed.c - very minimalist version of sed
 *
 * Copyright (C) 1999,2000,2001 by Lineo, inc. and Mark Whitley
 * Copyright (C) 1999,2000,2001 by Mark Whitley <markw@codepoet.org>
 * Copyright (C) 2002  Matt Kraai
 * Copyright (C) 2003 by Glenn McGrath <bug1@iinet.net.au>
 * Copyright (C) 2003,2004 by Rob Landley <rob@landley.net>
 *
 * MAINTAINER: Rob Landley <rob@landley.net>
 *
 * Licensed under GPLv2, see file LICENSE in this tarball for details.
 */

/* Code overview.

  Files are laid out to avoid unnecessary function declarations.  So for
  example, every function add_cmd calls occurs before add_cmd in this file.

  add_cmd() is called on each line of sed command text (from a file or from
  the command line).  It calls get_address() and parse_cmd_args().  The
  resulting sed_cmd_t structures are appended to a linked list
  (bbg.sed_cmd_head/bbg.sed_cmd_tail).

  add_input_file() adds a FILE * to the list of input files.  We need to
  know all input sources ahead of time to find the last line for the $ match.

  process_files() does actual sedding, reading data lines from each input FILE *
  (which could be stdin) and applying the sed command list (sed_cmd_head) to
  each of the resulting lines.

  sed_main() is where external code calls into this, with a command line.
*/


/*
	Supported features and commands in this version of sed:

	 - comments ('#')
	 - address matching: num|/matchstr/[,num|/matchstr/|$]command
	 - commands: (p)rint, (d)elete, (s)ubstitue (with g & I flags)
	 - edit commands: (a)ppend, (i)nsert, (c)hange
	 - file commands: (r)ead
	 - backreferences in substitution expressions (\0, \1, \2...\9)
	 - grouped commands: {cmd1;cmd2}
	 - transliteration (y/source-chars/dest-chars/)
	 - pattern space hold space storing / swapping (g, h, x)
	 - labels / branching (: label, b, t, T)

	 (Note: Specifying an address (range) to match is *optional*; commands
	 default to the whole pattern space if no specific address match was
	 requested.)

	Todo:
	 - Create a wrapper around regex to make libc's regex conform with sed

	Reference http://www.opengroup.org/onlinepubs/007904975/utilities/sed.html
*/

#include "busybox.h"
#include "xregex.h"

/* Each sed command turns into one of these structures. */
typedef struct sed_cmd_s {
    /* Ordered by alignment requirements: currently 36 bytes on x86 */

    /* address storage */
    regex_t *beg_match;	/* sed -e '/match/cmd' */
    regex_t *end_match;	/* sed -e '/match/,/end_match/cmd' */
    regex_t *sub_match;	/* For 's/sub_match/string/' */
    int beg_line;		/* 'sed 1p'   0 == apply commands to all lines */
    int end_line;		/* 'sed 1,3p' 0 == one line only. -1 = last line ($) */

    FILE *file;			/* File (sw) command writes to, -1 for none. */
    char *string;		/* Data string for (saicytb) commands. */

    unsigned short which_match;		/* (s) Which match to replace (0 for all) */

    /* Bitfields (gcc won't group them if we don't) */
    unsigned int invert:1;			/* the '!' after the address */
    unsigned int in_match:1;		/* Next line also included in match? */
    unsigned int no_newline:1;		/* Last line written by (sw) had no '\n' */
    unsigned int sub_p:1;			/* (s) print option */

    /* GENERAL FIELDS */
    char cmd;				/* The command char: abcdDgGhHilnNpPqrstwxy:={} */
    struct sed_cmd_s *next;	/* Next command (linked list, NULL terminated) */
} sed_cmd_t;

static const char *const semicolon_whitespace = "; \n\r\t\v";

struct sed_globals
{
	/* options */
	int be_quiet, in_place, regex_type;
	FILE *nonstdout;
	char *outname, *hold_space;

	/* List of input files */
	int input_file_count,current_input_file;
	FILE **input_file_list;

	regmatch_t regmatch[10];
	regex_t *previous_regex_ptr;
	
	/* linked list of sed commands */
	sed_cmd_t sed_cmd_head, *sed_cmd_tail;

	/* Linked list of append lines */
	llist_t *append_head;

	char *add_cmd_line;

	struct pipeline {
		char *buf;	/* Space to hold string */
		int idx;	/* Space used */
		int len;	/* Space allocated */
	} pipeline;
} bbg;


void sed_free_and_close_stuff(void);
#if ENABLE_FEATURE_CLEAN_UP
void sed_free_and_close_stuff(void)
{
	sed_cmd_t *sed_cmd = bbg.sed_cmd_head.next;

	llist_free(bbg.append_head, free);

	while (sed_cmd) {
		sed_cmd_t *sed_cmd_next = sed_cmd->next;

		if(sed_cmd->file)
			bb_xprint_and_close_file(sed_cmd->file);

		if (sed_cmd->beg_match) {
			regfree(sed_cmd->beg_match);
			free(sed_cmd->beg_match);
		}
		if (sed_cmd->end_match) {
			regfree(sed_cmd->end_match);
			free(sed_cmd->end_match);
		}
		if (sed_cmd->sub_match) {
			regfree(sed_cmd->sub_match);
			free(sed_cmd->sub_match);
		}
		free(sed_cmd->string);
		free(sed_cmd);
		sed_cmd = sed_cmd_next;
	}

	if(bbg.hold_space) free(bbg.hold_space);

    while(bbg.current_input_file<bbg.input_file_count)
		fclose(bbg.input_file_list[bbg.current_input_file++]);
}
#endif

/* If something bad happens during -i operation, delete temp file */

static void cleanup_outname(void)
{
  if(bbg.outname) unlink(bbg.outname);
}

/* strdup, replacing "\n" with '\n', and "\delimiter" with 'delimiter' */

static void parse_escapes(char *dest, char *string, int len, char from, char to)
{
	int i=0;

	while(i<len) {
		if(string[i] == '\\') {
			if(!to || string[i+1] == from) {
				*(dest++) = to ? to : string[i+1];
				i+=2;
				continue;
			} else *(dest++)=string[i++];
		}
		*(dest++) = string[i++];
	}
	*dest=0;
}

static char *copy_parsing_escapes(char *string, int len)
{
	char *dest=xmalloc(len+1);

	parse_escapes(dest,string,len,'n','\n');
	return dest;
}


/*
 * index_of_next_unescaped_regexp_delim - walks left to right through a string
 * beginning at a specified index and returns the index of the next regular
 * expression delimiter (typically a forward * slash ('/')) not preceded by
 * a backslash ('\').  A negative delimiter disables square bracket checking.
 */
static int index_of_next_unescaped_regexp_delim(int delimiter, char *str)
{
	int bracket = -1;
	int escaped = 0;
	int idx = 0;
	char ch;

	if (delimiter < 0) {
		bracket--;
		delimiter *= -1;
	}

	for (; (ch = str[idx]); idx++) {
		if (bracket >= 0) {
			if (ch == ']' && !(bracket == idx - 1 || (bracket == idx - 2
					&& str[idx - 1] == '^')))
				bracket = -1;
		} else if (escaped)
			escaped = 0;
		else if (ch == '\\')
			escaped = 1;
		else if (bracket == -1 && ch == '[')
			bracket = idx;
		else if (ch == delimiter)
			return idx;
	}

	/* if we make it to here, we've hit the end of the string */
	bb_error_msg_and_die("unmatched '%c'",delimiter);
}

/*
 *  Returns the index of the third delimiter
 */
static int parse_regex_delim(char *cmdstr, char **match, char **replace)
{
	char *cmdstr_ptr = cmdstr;
	char delimiter;
	int idx = 0;

	/* verify that the 's' or 'y' is followed by something.  That something
	 * (typically a 'slash') is now our regexp delimiter... */
	if (*cmdstr == '\0')
		bb_error_msg_and_die("bad format in substitution expression");
	delimiter = *(cmdstr_ptr++);

	/* save the match string */
	idx = index_of_next_unescaped_regexp_delim(delimiter, cmdstr_ptr);
	*match = copy_parsing_escapes(cmdstr_ptr, idx);

	/* save the replacement string */
	cmdstr_ptr += idx + 1;
	idx = index_of_next_unescaped_regexp_delim(-delimiter, cmdstr_ptr);
	*replace = copy_parsing_escapes(cmdstr_ptr, idx);

	return ((cmdstr_ptr - cmdstr) + idx);
}

/*
 * returns the index in the string just past where the address ends.
 */
static int get_address(char *my_str, int *linenum, regex_t ** regex)
{
	char *pos = my_str;

	if (isdigit(*my_str)) {
		*linenum = strtol(my_str, &pos, 10);
		/* endstr shouldnt ever equal NULL */
	} else if (*my_str == '$') {
		*linenum = -1;
		pos++;
	} else if (*my_str == '/' || *my_str == '\\') {
		int next;
		char delimiter;
		char *temp;

		if (*my_str == '\\') delimiter = *(++pos);
		else delimiter = '/';
		next = index_of_next_unescaped_regexp_delim(delimiter, ++pos);
		temp = copy_parsing_escapes(pos,next);
		*regex = (regex_t *) xmalloc(sizeof(regex_t));
		xregcomp(*regex, temp, bbg.regex_type|REG_NEWLINE);
		free(temp);
		/* Move position to next character after last delimiter */
		pos += (next+1);
	}
	return pos - my_str;
}

/* Grab a filename.  Whitespace at start is skipped, then goes to EOL. */
static int parse_file_cmd(sed_cmd_t *sed_cmd, char *filecmdstr, char **retval)
{
	int start = 0, idx, hack=0;

	/* Skip whitespace, then grab filename to end of line */
	while (isspace(filecmdstr[start])) start++;
	idx=start;
	while(filecmdstr[idx] && filecmdstr[idx]!='\n') idx++;
	/* If lines glued together, put backslash back. */
	if(filecmdstr[idx]=='\n') hack=1;
	if(idx==start) bb_error_msg_and_die("Empty filename");
	*retval = bb_xstrndup(filecmdstr+start, idx-start+hack+1);
	if(hack) *(idx+*retval)='\\';

	return idx;
}

static int parse_subst_cmd(sed_cmd_t *sed_cmd, char *substr)
{
	int cflags = bbg.regex_type;
	char *match;
	int idx = 0;

	/*
	 * A substitution command should look something like this:
	 *    s/match/replace/ #gIpw
	 *    ||     |        |||
	 *    mandatory       optional
	 */
	idx = parse_regex_delim(substr, &match, &sed_cmd->string);

	/* determine the number of back references in the match string */
	/* Note: we compute this here rather than in the do_subst_command()
	 * function to save processor time, at the expense of a little more memory
	 * (4 bits) per sed_cmd */

	/* process the flags */

	sed_cmd->which_match=1;
	while (substr[++idx]) {
		/* Parse match number */
		if(isdigit(substr[idx])) {
			if(match[0]!='^') {
				/* Match 0 treated as all, multiple matches we take the last one. */
				char *pos=substr+idx;
				sed_cmd->which_match=(unsigned short)strtol(substr+idx,&pos,10);
				idx=pos-substr;
			}
			continue;
		}
		/* Skip spaces */
		if(isspace(substr[idx])) continue;

		switch (substr[idx]) {
			/* Replace all occurrences */
			case 'g':
				if (match[0] != '^') sed_cmd->which_match = 0;
				break;
			/* Print pattern space */
			case 'p':
				sed_cmd->sub_p = 1;
				break;
			/* Write to file */
			case 'w':
			{
				char *temp;
				idx+=parse_file_cmd(sed_cmd,substr+idx,&temp);

				break;
			}
			/* Ignore case (gnu exension) */
			case 'I':
				cflags |= REG_ICASE;
				break;
			/* Comment */
			case '#':
				while(substr[++idx]);
				/* Fall through */
			/* End of command */
			case ';':
			case '}':
				goto out;
			default:
				bb_error_msg_and_die("bad option in substitution expression");
		}
	}
out:
	/* compile the match string into a regex */
	if (*match != '\0') {
		/* If match is empty, we use last regex used at runtime */
		sed_cmd->sub_match = (regex_t *) xmalloc(sizeof(regex_t));
		xregcomp(sed_cmd->sub_match, match, cflags);
	}
	free(match);

	return idx;
}

/*
 *  Process the commands arguments
 */
static char *parse_cmd_args(sed_cmd_t *sed_cmd, char *cmdstr)
{
	/* handle (s)ubstitution command */
	if (sed_cmd->cmd == 's') cmdstr += parse_subst_cmd(sed_cmd, cmdstr);
	/* handle edit cmds: (a)ppend, (i)nsert, and (c)hange */
	else if (strchr("aic", sed_cmd->cmd)) {
		if ((sed_cmd->end_line || sed_cmd->end_match) && sed_cmd->cmd != 'c')
			bb_error_msg_and_die
				("only a beginning address can be specified for edit commands");
		for(;;) {
			if(*cmdstr=='\n' || *cmdstr=='\\') {
				cmdstr++;
				break;
			} else if(isspace(*cmdstr)) cmdstr++;
			else break;
		}
		sed_cmd->string = bb_xstrdup(cmdstr);
		parse_escapes(sed_cmd->string,sed_cmd->string,strlen(cmdstr),0,0);
		cmdstr += strlen(cmdstr);
	/* handle file cmds: (r)ead */
	} else if(strchr("rw", sed_cmd->cmd)) {
		if (sed_cmd->end_line || sed_cmd->end_match)
			bb_error_msg_and_die("Command only uses one address");
		cmdstr += parse_file_cmd(sed_cmd, cmdstr, &sed_cmd->string);
		if(sed_cmd->cmd=='w')
			sed_cmd->file=bb_xfopen(sed_cmd->string,"w");
	/* handle branch commands */
	} else if (strchr(":btT", sed_cmd->cmd)) {
		int length;

		while(isspace(*cmdstr)) cmdstr++;
		length = strcspn(cmdstr, semicolon_whitespace);
		if (length) {
			sed_cmd->string = bb_xstrndup(cmdstr, length);
			cmdstr += length;
		}
	}
	/* translation command */
	else if (sed_cmd->cmd == 'y') {
		char *match, *replace;
		int i=cmdstr[0];

		cmdstr+=parse_regex_delim(cmdstr, &match, &replace)+1;
		/* \n already parsed, but \delimiter needs unescaping. */
		parse_escapes(match,match,strlen(match),i,i);
		parse_escapes(replace,replace,strlen(replace),i,i);

		sed_cmd->string = xzalloc((strlen(match) + 1) * 2);
		for (i = 0; match[i] && replace[i]; i++) {
			sed_cmd->string[i * 2] = match[i];
			sed_cmd->string[(i * 2) + 1] = replace[i];
		}
		free(match);
		free(replace);
	}
	/* if it wasnt a single-letter command that takes no arguments
	 * then it must be an invalid command.
	 */
	else if (strchr("dDgGhHlnNpPqx={}", sed_cmd->cmd) == 0) {
		bb_error_msg_and_die("Unsupported command %c", sed_cmd->cmd);
	}

	/* give back whatever's left over */
	return (cmdstr);
}


/* Parse address+command sets, skipping comment lines. */

static void add_cmd(char *cmdstr)
{
	sed_cmd_t *sed_cmd;
	int temp;

	/* Append this line to any unfinished line from last time. */
	if (bbg.add_cmd_line) {
		cmdstr = bb_xasprintf("%s\n%s", bbg.add_cmd_line, cmdstr);
		free(bbg.add_cmd_line);
		bbg.add_cmd_line = cmdstr;
	}

	/* If this line ends with backslash, request next line. */
	temp=strlen(cmdstr);
	if(temp && cmdstr[temp-1]=='\\') {
		if (!bbg.add_cmd_line) bbg.add_cmd_line = bb_xstrdup(cmdstr);
		bbg.add_cmd_line[temp-1] = 0;
		return;
	}

	/* Loop parsing all commands in this line. */
	while(*cmdstr) {
		/* Skip leading whitespace and semicolons */
		cmdstr += strspn(cmdstr, semicolon_whitespace);

		/* If no more commands, exit. */
		if(!*cmdstr) break;

		/* if this is a comment, jump past it and keep going */
		if (*cmdstr == '#') {
			/* "#n" is the same as using -n on the command line */
			if (cmdstr[1] == 'n') bbg.be_quiet++;
			if(!(cmdstr=strpbrk(cmdstr, "\n\r"))) break;
			continue;
		}

		/* parse the command
		 * format is: [addr][,addr][!]cmd
		 *            |----||-----||-|
		 *            part1 part2  part3
		 */

		sed_cmd = xzalloc(sizeof(sed_cmd_t));

		/* first part (if present) is an address: either a '$', a number or a /regex/ */
		cmdstr += get_address(cmdstr, &sed_cmd->beg_line, &sed_cmd->beg_match);

		/* second part (if present) will begin with a comma */
		if (*cmdstr == ',') {
			int idx;

			cmdstr++;
			idx = get_address(cmdstr, &sed_cmd->end_line, &sed_cmd->end_match);
			if (!idx) bb_error_msg_and_die("no address after comma\n");
			cmdstr += idx;
		}

		/* skip whitespace before the command */
		while (isspace(*cmdstr)) cmdstr++;

		/* Check for inversion flag */
		if (*cmdstr == '!') {
			sed_cmd->invert = 1;
			cmdstr++;

			/* skip whitespace before the command */
			while (isspace(*cmdstr)) cmdstr++;
		}

		/* last part (mandatory) will be a command */
		if (!*cmdstr) bb_error_msg_and_die("missing command");
		sed_cmd->cmd = *(cmdstr++);
		cmdstr = parse_cmd_args(sed_cmd, cmdstr);

		/* Add the command to the command array */
		bbg.sed_cmd_tail->next = sed_cmd;
		bbg.sed_cmd_tail = bbg.sed_cmd_tail->next;
	}

	/* If we glued multiple lines together, free the memory. */
	free(bbg.add_cmd_line);
	bbg.add_cmd_line = NULL;
}

/* Append to a string, reallocating memory as necessary. */

#define PIPE_GROW 64

static void pipe_putc(char c)
{
	if(bbg.pipeline.idx==bbg.pipeline.len) {
		bbg.pipeline.buf = xrealloc(bbg.pipeline.buf,
								bbg.pipeline.len + PIPE_GROW);
		bbg.pipeline.len+=PIPE_GROW;
	}
	bbg.pipeline.buf[bbg.pipeline.idx++] = c;
}

static void do_subst_w_backrefs(char *line, char *replace)
{
	int i,j;

	/* go through the replacement string */
	for (i = 0; replace[i]; i++) {
		/* if we find a backreference (\1, \2, etc.) print the backref'ed * text */
		if (replace[i] == '\\' && replace[i+1]>='0' && replace[i+1]<='9') {
			int backref=replace[++i]-'0';

			/* print out the text held in bbg.regmatch[backref] */
			if(bbg.regmatch[backref].rm_so != -1)
				for (j = bbg.regmatch[backref].rm_so;
					j < bbg.regmatch[backref].rm_eo; j++) pipe_putc(line[j]);
		}

		/* if we find a backslash escaped character, print the character */
		else if (replace[i] == '\\') pipe_putc(replace[++i]);

		/* if we find an unescaped '&' print out the whole matched text. */
		else if (replace[i] == '&')
			for (j = bbg.regmatch[0].rm_so; j < bbg.regmatch[0].rm_eo; j++)
				pipe_putc(line[j]);
		/* Otherwise just output the character. */
		else pipe_putc(replace[i]);
	}
}

static int do_subst_command(sed_cmd_t *sed_cmd, char **line)
{
	char *oldline = *line;
	int altered = 0;
	int match_count=0;
	regex_t *current_regex;

	/* Handle empty regex. */
	if (sed_cmd->sub_match == NULL) {
		current_regex = bbg.previous_regex_ptr;
		if(!current_regex)
			bb_error_msg_and_die("No previous regexp.");
	} else bbg.previous_regex_ptr = current_regex = sed_cmd->sub_match;

	/* Find the first match */
	if(REG_NOMATCH==regexec(current_regex, oldline, 10, bbg.regmatch, 0))
		return 0;

	/* Initialize temporary output buffer. */
	bbg.pipeline.buf=xmalloc(PIPE_GROW);
	bbg.pipeline.len=PIPE_GROW;
	bbg.pipeline.idx=0;

	/* Now loop through, substituting for matches */
	do {
		int i;

		/* Work around bug in glibc regexec, demonstrated by:
		   echo " a.b" | busybox sed 's [^ .]* x g'
		   The match_count check is so not to break
		   echo "hi" | busybox sed 's/^/!/g' */
		if(!bbg.regmatch[0].rm_so && !bbg.regmatch[0].rm_eo && match_count) {
			pipe_putc(*(oldline++));
			continue;
		}

		match_count++;

		/* If we aren't interested in this match, output old line to
		   end of match and continue */
		if(sed_cmd->which_match && sed_cmd->which_match!=match_count) {
			for(i=0;i<bbg.regmatch[0].rm_eo;i++)
				pipe_putc(*(oldline++));
			continue;
		}

		/* print everything before the match */
		for (i = 0; i < bbg.regmatch[0].rm_so; i++) pipe_putc(oldline[i]);

		/* then print the substitution string */
		do_subst_w_backrefs(oldline, sed_cmd->string);

		/* advance past the match */
		oldline += bbg.regmatch[0].rm_eo;
		/* flag that something has changed */
		altered++;

		/* if we're not doing this globally, get out now */
		if (sed_cmd->which_match) break;
	} while (*oldline && (regexec(current_regex, oldline, 10, bbg.regmatch, 0) != REG_NOMATCH));

	/* Copy rest of string into output pipeline */

	while(*oldline) pipe_putc(*(oldline++));
	pipe_putc(0);

	free(*line);
	*line = bbg.pipeline.buf;
	return altered;
}

/* Set command pointer to point to this label.  (Does not handle null label.) */
static sed_cmd_t *branch_to(char *label)
{
	sed_cmd_t *sed_cmd;

	for (sed_cmd = bbg.sed_cmd_head.next; sed_cmd; sed_cmd = sed_cmd->next) {
		if ((sed_cmd->cmd == ':') && (sed_cmd->string) && (strcmp(sed_cmd->string, label) == 0)) {
			return (sed_cmd);
		}
	}
	bb_error_msg_and_die("Can't find label for jump to `%s'", label);
}

static void append(char *s)
{
	llist_add_to_end(&bbg.append_head, bb_xstrdup(s));
}

static void flush_append(void)
{
	char *data;

	/* Output appended lines. */
	while((data = (char *)llist_pop(&bbg.append_head))) {
		fprintf(bbg.nonstdout,"%s\n",data);
		free(data);
	}
}

static void add_input_file(FILE *file)
{
	bbg.input_file_list=xrealloc(bbg.input_file_list,
			(bbg.input_file_count + 1) * sizeof(FILE *));
	bbg.input_file_list[bbg.input_file_count++] = file;
}

/* Get next line of input from bbg.input_file_list, flushing append buffer and
 * noting if we ran out of files without a newline on the last line we read.
 */
static char *get_next_line(int *no_newline)
{
	char *temp=NULL;
	int len;

	flush_append();
	while (bbg.current_input_file<bbg.input_file_count) {
		temp = bb_get_chunk_from_file(bbg.input_file_list[bbg.current_input_file],&len);
		if (temp) {
			*no_newline = !(len && temp[len-1]=='\n');
			if (!*no_newline) temp[len-1] = 0;
			break;
		// Close this file and advance to next one
		} else fclose(bbg.input_file_list[bbg.current_input_file++]);
	}

	return temp;
}

/* Output line of text.  missing_newline means the last line output did not
   end with a newline.  no_newline means this line does not end with a
   newline. */

static int puts_maybe_newline(char *s, FILE *file, int missing_newline, int no_newline)
{
	if(missing_newline) fputc('\n',file);
	fputs(s,file);
	if(!no_newline) fputc('\n',file);

    if(ferror(file)) {
		bb_default_error_retval = 4;  /* It's what gnu sed exits with... */
		bb_error_msg_and_die(bb_msg_write_error);
	}

	return no_newline;
}

#define sed_puts(s,n) missing_newline=puts_maybe_newline(s,bbg.nonstdout,missing_newline,n)

/* Process all the lines in all the files */

static void process_files(void)
{
	char *pattern_space, *next_line;
	int linenum = 0, missing_newline=0;
	int no_newline,next_no_newline=0;

	/* Prime the pump */
	next_line = get_next_line(&next_no_newline);

	/* go through every line in each file */
	for(;;) {
		sed_cmd_t *sed_cmd;
		int substituted=0;

		/* Advance to next line.  Stop if out of lines. */
		if(!(pattern_space=next_line)) break;
		no_newline=next_no_newline;

		/* Read one line in advance so we can act on the last line,
		 * the '$' address */
		next_line = get_next_line(&next_no_newline);
		linenum++;
restart:
		/* for every line, go through all the commands */
		for (sed_cmd = bbg.sed_cmd_head.next; sed_cmd; sed_cmd = sed_cmd->next)
		{
			int old_matched, matched;

			old_matched = sed_cmd->in_match;

			/* Determine if this command matches this line: */

			/* Are we continuing a previous multi-line match? */

			sed_cmd->in_match = sed_cmd->in_match

			/* Or is no range necessary? */
				|| (!sed_cmd->beg_line && !sed_cmd->end_line
					&& !sed_cmd->beg_match && !sed_cmd->end_match)

			/* Or did we match the start of a numerical range? */
				|| (sed_cmd->beg_line > 0 && (sed_cmd->beg_line == linenum))

			/* Or does this line match our begin address regex? */
			        || (sed_cmd->beg_match &&
				    !regexec(sed_cmd->beg_match, pattern_space, 0, NULL, 0))

			/* Or did we match last line of input? */
				|| (sed_cmd->beg_line == -1 && next_line == NULL);

			/* Snapshot the value */

			matched = sed_cmd->in_match;

			/* Is this line the end of the current match? */

			if(matched) {
				sed_cmd->in_match = !(
					/* has the ending line come, or is this a single address command? */
					(sed_cmd->end_line ?
						sed_cmd->end_line==-1 ?
							!next_line
							: sed_cmd->end_line<=linenum
						: !sed_cmd->end_match)
					/* or does this line matches our last address regex */
					|| (sed_cmd->end_match && old_matched && (regexec(sed_cmd->end_match, pattern_space, 0, NULL, 0) == 0))
				);
			}

			/* Skip blocks of commands we didn't match. */
			if (sed_cmd->cmd == '{') {
				if(sed_cmd->invert ? matched : !matched)
					while(sed_cmd && sed_cmd->cmd!='}') sed_cmd=sed_cmd->next;
				if(!sed_cmd) bb_error_msg_and_die("Unterminated {");
				continue;
			}

			/* Okay, so did this line match? */
			if (sed_cmd->invert ? !matched : matched) {
				/* Update last used regex in case a blank substitute BRE is found */
				if (sed_cmd->beg_match) {
					bbg.previous_regex_ptr = sed_cmd->beg_match;
				}

				/* actual sedding */
				switch (sed_cmd->cmd) {

					/* Print line number */
					case '=':
						fprintf(bbg.nonstdout,"%d\n", linenum);
						break;

					/* Write the current pattern space up to the first newline */
					case 'P':
					{
						char *tmp = strchr(pattern_space, '\n');

						if (tmp) {
							*tmp = '\0';
							sed_puts(pattern_space,1);
							*tmp = '\n';
							break;
						}
						/* Fall Through */
					}

					/* Write the current pattern space to output */
					case 'p':
						sed_puts(pattern_space,no_newline);
						break;
					/* Delete up through first newline */
					case 'D':
					{
						char *tmp = strchr(pattern_space,'\n');

						if(tmp) {
							tmp=bb_xstrdup(tmp+1);
							free(pattern_space);
							pattern_space=tmp;
							goto restart;
						}
					}
					/* discard this line. */
					case 'd':
						goto discard_line;

					/* Substitute with regex */
					case 's':
						if(do_subst_command(sed_cmd, &pattern_space)) {
							substituted|=1;

							/* handle p option */
							if(sed_cmd->sub_p)
								sed_puts(pattern_space,no_newline);
							/* handle w option */
							if(sed_cmd->file)
								sed_cmd->no_newline=puts_maybe_newline(pattern_space, sed_cmd->file, sed_cmd->no_newline, no_newline);

						}
						break;

					/* Append line to linked list to be printed later */
					case 'a':
					{
						append(sed_cmd->string);
						break;
					}

					/* Insert text before this line */
					case 'i':
						sed_puts(sed_cmd->string,1);
						break;

					/* Cut and paste text (replace) */
					case 'c':
						/* Only triggers on last line of a matching range. */
						if (!sed_cmd->in_match) sed_puts(sed_cmd->string,0);
						goto discard_line;

					/* Read file, append contents to output */
					case 'r':
					{
						FILE *rfile;

						rfile = fopen(sed_cmd->string, "r");
						if (rfile) {
							char *line;

							while ((line = bb_get_chomped_line_from_file(rfile))
									!= NULL)
								append(line);
							bb_xprint_and_close_file(rfile);
						}

						break;
					}

					/* Write pattern space to file. */
					case 'w':
						sed_cmd->no_newline=puts_maybe_newline(pattern_space,sed_cmd->file, sed_cmd->no_newline,no_newline);
						break;

					/* Read next line from input */
					case 'n':
						if (!bbg.be_quiet)
							sed_puts(pattern_space,no_newline);
						if (next_line) {
							free(pattern_space);
							pattern_space = next_line;
							no_newline=next_no_newline;
							next_line = get_next_line(&next_no_newline);
							linenum++;
							break;
						}
						/* fall through */

					/* Quit.  End of script, end of input. */
					case 'q':
						/* Exit the outer while loop */
						free(next_line);
						next_line = NULL;
						goto discard_commands;

					/* Append the next line to the current line */
					case 'N':
					{
						/* If no next line, jump to end of script and exit. */
						if (next_line == NULL) {
							/* Jump to end of script and exit */
							free(next_line);
							next_line = NULL;
							goto discard_line;
						/* append next_line, read new next_line. */
						} else {
							int len=strlen(pattern_space);

							pattern_space = realloc(pattern_space, len + strlen(next_line) + 2);
							pattern_space[len]='\n';
							strcpy(pattern_space+len+1, next_line);
							no_newline=next_no_newline;
							next_line = get_next_line(&next_no_newline);
							linenum++;
						}
						break;
					}

					/* Test/branch if substitution occurred */
					case 't':
						if(!substituted) break;
						substituted=0;
						/* Fall through */
					/* Test/branch if substitution didn't occur */
					case 'T':
						if (substituted) break;
						/* Fall through */
					/* Branch to label */
					case 'b':
						if (!sed_cmd->string) goto discard_commands;
						else sed_cmd = branch_to(sed_cmd->string);
						break;
					/* Transliterate characters */
					case 'y':
					{
						int i;

						for (i = 0; pattern_space[i]; i++) {
							int j;

							for (j = 0; sed_cmd->string[j]; j += 2) {
								if (pattern_space[i] == sed_cmd->string[j]) {
									pattern_space[i] = sed_cmd->string[j + 1];
									break;
								}
							}
						}

						break;
					}
					case 'g':	/* Replace pattern space with hold space */
						free(pattern_space);
						pattern_space = bb_xstrdup(bbg.hold_space ? bbg.hold_space : "");
						break;
					case 'G':	/* Append newline and hold space to pattern space */
					{
						int pattern_space_size = 2;
						int hold_space_size = 0;

						if (pattern_space)
							pattern_space_size += strlen(pattern_space);
						if (bbg.hold_space)
							hold_space_size = strlen(bbg.hold_space);
						pattern_space = xrealloc(pattern_space,
								pattern_space_size + hold_space_size);
						if (pattern_space_size == 2) pattern_space[0]=0;
						strcat(pattern_space, "\n");
						if (bbg.hold_space)
							strcat(pattern_space, bbg.hold_space);
						no_newline=0;

						break;
					}
					case 'h':	/* Replace hold space with pattern space */
						free(bbg.hold_space);
						bbg.hold_space = bb_xstrdup(pattern_space);
						break;
					case 'H':	/* Append newline and pattern space to hold space */
					{
						int hold_space_size = 2;
						int pattern_space_size = 0;

						if (bbg.hold_space)
							hold_space_size += strlen(bbg.hold_space);
						if (pattern_space)
							pattern_space_size = strlen(pattern_space);
						bbg.hold_space = xrealloc(bbg.hold_space,
											hold_space_size + pattern_space_size);

						if (hold_space_size == 2) *bbg.hold_space=0;
						strcat(bbg.hold_space, "\n");
						if (pattern_space) strcat(bbg.hold_space, pattern_space);

						break;
					}
					case 'x': /* Exchange hold and pattern space */
					{
						char *tmp = pattern_space;
						pattern_space = bbg.hold_space ? : xzalloc(1);
						no_newline=0;
						bbg.hold_space = tmp;
						break;
					}
				}
			}
		}

		/*
		 * exit point from sedding...
		 */
discard_commands:
		/* we will print the line unless we were told to be quiet ('-n')
		   or if the line was suppressed (ala 'd'elete) */
		if (!bbg.be_quiet) sed_puts(pattern_space,no_newline);

		/* Delete and such jump here. */
discard_line:
		flush_append();
		free(pattern_space);
	}
}

/* It is possible to have a command line argument with embedded
   newlines.  This counts as multiple command lines. */

static void add_cmd_block(char *cmdstr)
{
	int go=1;
	char *temp=bb_xstrdup(cmdstr),*temp2=temp;

	while(go) {
		int len=strcspn(temp2,"\n");
		if(!temp2[len]) go=0;
		else temp2[len]=0;
		add_cmd(temp2);
		temp2+=len+1;
	}
	free(temp);
}

int sed_main(int argc, char **argv)
{
	int status = EXIT_SUCCESS, opt, getpat = 1;

	bbg.sed_cmd_tail=&bbg.sed_cmd_head;

	/* destroy command strings on exit */
	if (ENABLE_FEATURE_CLEAN_UP) atexit(sed_free_and_close_stuff);

	/* Lie to autoconf when it starts asking stupid questions. */
	if(argc==2 && !strcmp(argv[1],"--version")) {
		printf("This is not GNU sed version 4.0\n");
		exit(0);
	}

	/* do normal option parsing */
	while ((opt = getopt(argc, argv, "irne:f:")) > 0) {
		switch (opt) {
		case 'i':
			bbg.in_place++;
			atexit(cleanup_outname);
			break;
		case 'r':
			bbg.regex_type|=REG_EXTENDED;
			break;
		case 'n':
			bbg.be_quiet++;
			break;
		case 'e':
			add_cmd_block(optarg);
			getpat=0;
			break;
		case 'f':
		{
			FILE *cmdfile;
			char *line;

			cmdfile = bb_xfopen(optarg, "r");

			while ((line = bb_get_chomped_line_from_file(cmdfile)) != NULL) {
				add_cmd(line);
				getpat=0;
				free(line);
			}
			bb_xprint_and_close_file(cmdfile);

			break;
		}
		default:
			bb_show_usage();
		}
	}

	/* if we didn't get a pattern from -e or -f, use argv[optind] */
	if(getpat) {
		if (argv[optind] == NULL)
			bb_show_usage();
		else
			add_cmd_block(argv[optind++]);
	}
	/* Flush any unfinished commands. */
	add_cmd("");

	/* By default, we write to stdout */
	bbg.nonstdout=stdout;

	/* argv[(optind)..(argc-1)] should be names of file to process. If no
	 * files were specified or '-' was specified, take input from stdin.
	 * Otherwise, we process all the files specified. */
	if (argv[optind] == NULL) {
		if(bbg.in_place) bb_error_msg_and_die(bb_msg_requires_arg, "-i");
		add_input_file(stdin);
		process_files();
	} else {
		int i;
		FILE *file;

		for (i = optind; i < argc; i++) {
			if(!strcmp(argv[i], "-") && !bbg.in_place) {
				add_input_file(stdin);
				process_files();
			} else {
				file = bb_wfopen(argv[i], "r");
				if (file) {
					if(bbg.in_place) {
						struct stat statbuf;
						int nonstdoutfd;

						bbg.outname=bb_xstrndup(argv[i],strlen(argv[i])+6);
						strcat(bbg.outname,"XXXXXX");
						if(-1==(nonstdoutfd=mkstemp(bbg.outname)))
							bb_error_msg_and_die("no temp file");
						bbg.nonstdout=fdopen(nonstdoutfd,"w");

						/* Set permissions of output file */

						fstat(fileno(file),&statbuf);
						fchmod(nonstdoutfd,statbuf.st_mode);
						add_input_file(file);
						process_files();
						fclose(bbg.nonstdout);

						bbg.nonstdout=stdout;
						unlink(argv[i]);
						rename(bbg.outname,argv[i]);
						free(bbg.outname);
						bbg.outname=0;
					} else add_input_file(file);
				} else {
					status = EXIT_FAILURE;
				}
			}
		}
		if(bbg.input_file_count>bbg.current_input_file) process_files();
	}

	return status;
}
