/** @file
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 2001 Gerald Combs
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef DFILTER_INT_H
#define DFILTER_INT_H

#include "dfilter.h"
#include "syntax-tree.h"

#include <epan/proto.h>
#include <stdio.h>

typedef struct {
	const header_field_info *hfinfo;
	fvalue_t *value;
	int proto_layer_num;
} df_reference_t;

/* Passed back to user */
struct epan_dfilter {
	GPtrArray	*insns;
	guint		num_registers;
	GSList		**registers;
	gboolean	*attempted_load;
	GDestroyNotify	*free_registers;
	int		*interesting_fields;
	int		num_interesting_fields;
	GPtrArray	*deprecated;
	char		*expanded_text;
	GHashTable	*references;
	GHashTable	*raw_references;
	char		*syntax_tree_str;
	/* Used to pass arguments to functions. List of Lists (list of registers). */
	GSList		*function_stack;
};

typedef struct {
	/* Syntax Tree stuff */
	stnode_t	*st_root;
	gboolean	parse_failure;
	df_error_t	error;
	GPtrArray	*insns;
	GHashTable	*loaded_fields;
	GHashTable	*loaded_raw_fields;
	GHashTable	*interesting_fields;
	int		next_insn_id;
	int		next_register;
	GPtrArray	*deprecated;
	GHashTable	*references; /* hfinfo -> pointer to array of references */
	GHashTable	*raw_references; /* hfinfo -> pointer to array of references */
	char		*expanded_text;
	gboolean	apply_optimization;
	wmem_allocator_t *dfw_scope; /* Because we use exceptions for error handling sometimes
	                                cleaning up memory allocations is inconvenient. Memory
					allocated from this pool will be freed when the dfwork_t
					context is destroyed. */
} dfwork_t;

/*
 * State kept by the scanner.
 */
typedef struct {
	dfwork_t *dfw;
	GString* quoted_string;
	gboolean raw_string;
	df_loc_t string_loc;
	df_loc_t location;
} df_scanner_state_t;

/* Constructor/Destructor prototypes for Lemon Parser */
void *DfilterAlloc(void* (*)(gsize));

void DfilterFree(void*, void (*)(void *));

void Dfilter(void*, int, stnode_t*, dfwork_t*);

/* Return value for error in scanner. */
#define SCAN_FAILED	-1	/* not 0, as that means end-of-input */

void
dfilter_vfail(dfwork_t *dfw, int code, df_loc_t err_loc,
			const char *format, va_list args);

void
dfilter_fail(dfwork_t *dfw, int code, df_loc_t err_loc,
			const char *format, ...) G_GNUC_PRINTF(4, 5);

WS_NORETURN
void
dfilter_fail_throw(dfwork_t *dfw, int code, df_loc_t err_loc,
			const char *format, ...) G_GNUC_PRINTF(4, 5);

void
dfw_set_error_location(dfwork_t *dfw, df_loc_t err_loc);

void
add_deprecated_token(dfwork_t *dfw, const char *token);

void
free_deprecated(GPtrArray *deprecated);

void
DfilterTrace(FILE *TraceFILE, char *zTracePrompt);

header_field_info *
dfilter_resolve_unparsed(dfwork_t *dfw, const char *name);

WS_RETNONNULL fvalue_t*
dfilter_fvalue_from_literal(dfwork_t *dfw, ftenum_t ftype, stnode_t *st,
		gboolean allow_partial_value, header_field_info *hfinfo_value_string);

WS_RETNONNULL fvalue_t *
dfilter_fvalue_from_string(dfwork_t *dfw, ftenum_t ftype, stnode_t *st,
		header_field_info *hfinfo_value_string);

WS_RETNONNULL fvalue_t *
dfilter_fvalue_from_charconst(dfwork_t *dfw, ftenum_t ftype, stnode_t *st);

const char *tokenstr(int token);

df_reference_t *
reference_new(const field_info *finfo, gboolean raw);

void
reference_free(df_reference_t *ref);

void dfw_error_init(df_error_t *err);

void dfw_error_clear(df_error_t *err);

void dfw_error_set_msg(df_error_t **errp, const char *fmt, ...)
G_GNUC_PRINTF(2, 3);

void dfw_error_take(df_error_t **errp, df_error_t *src);

#endif
