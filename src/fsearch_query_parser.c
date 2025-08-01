#include "fsearch_query_parser.h"
#include "fsearch_query_lexer.h"
#include "fsearch_query_node.h"
#include "fsearch_size_utils.h"
#include "fsearch_string_utils.h"
#include "fsearch_time_utils.h"

#include <stdbool.h>

typedef FsearchQueryNode *(FsearchQueryComparisonNewNodeFunc)(FsearchQueryFlags,
                                                              int64_t,
                                                              int64_t,
                                                              FsearchQueryNodeComparison);
typedef bool(FsearchQueryIntegerParserFunc)(const char *, int64_t *, int64_t *);

static GList *
parse_open_bracket(FsearchQueryParseContext *parse_ctx);

static GList *
parse_close_bracket(FsearchQueryParseContext *parse_ctx);

static GList *
parse_operator(FsearchQueryParseContext *parse_ctx, FsearchQueryToken token);

static GList *
parse_word(GString *field_name, FsearchQueryFlags flags);

static GList *
parse_field(FsearchQueryParseContext *parse_ctx, GString *field_name, bool is_empty_field, FsearchQueryFlags flags);

static GList *
parse_modifier(FsearchQueryParseContext *parse_ctx, bool is_empty_field, FsearchQueryFlags flags);

static GList *
parse_function_date_modified(FsearchQueryParseContext *parse_ctx, bool is_empty_field, FsearchQueryFlags flags);

static GList *
parse_function_depth(FsearchQueryParseContext *parse_ctx, bool is_empty_field, FsearchQueryFlags flags);

static GList *
parse_function_size(FsearchQueryParseContext *parse_ctx, bool is_empty_field, FsearchQueryFlags flags);

static GList *
parse_function_empty(FsearchQueryParseContext *parse_ctx, bool is_empty_field, FsearchQueryFlags flags);

static GList *
parse_function_childcount(FsearchQueryParseContext *parse_ctx, bool is_empty_field, FsearchQueryFlags flags);

static GList *
parse_function_childfilecount(FsearchQueryParseContext *parse_ctx, bool is_empty_field, FsearchQueryFlags flags);

static GList *
parse_function_childfoldercount(FsearchQueryParseContext *parse_ctx, bool is_empty_field, FsearchQueryFlags flags);

static GList *
parse_function_contenttype(FsearchQueryParseContext *parse_ctx, bool is_empty_field, FsearchQueryFlags flags);

static GList *
parse_function_extension(FsearchQueryParseContext *parse_ctx, bool is_empty_field, FsearchQueryFlags flags);

static GList *
parse_function_parent(FsearchQueryParseContext *parse_ctx, bool is_empty_field, FsearchQueryFlags flags);

static GList *
get_implicit_and_if_necessary(FsearchQueryParseContext *parse_ctx,
                              FsearchQueryToken last_token,
                              FsearchQueryToken next_token);

typedef GList *(FsearchTokenFunctionParser)(FsearchQueryParseContext *parse_ctx, bool, FsearchQueryFlags);

typedef enum {
    ADD_FLAG,
    REMOVE_FLAG,
} FsearchQueryFlagOperation;

typedef struct FsearchTokenModifier {
    const char *name;
    FsearchQueryFlags flag;
    FsearchQueryFlagOperation flag_operation;
} FsearchTokenModifier;

typedef struct FsearchTokenFunction {
    const char *name;
    FsearchTokenFunctionParser *parser;
} FsearchTokenFunction;

FsearchTokenModifier supported_modifiers[] = {
    {"case", QUERY_FLAG_MATCH_CASE, ADD_FLAG},
    {"nocase", QUERY_FLAG_MATCH_CASE, REMOVE_FLAG},
    {"exact", QUERY_FLAG_EXACT_MATCH, ADD_FLAG},
    {"file", QUERY_FLAG_FILES_ONLY, ADD_FLAG},
    {"nofileonly", QUERY_FLAG_FILES_ONLY, REMOVE_FLAG},
    {"files", QUERY_FLAG_FILES_ONLY, ADD_FLAG},
    {"nofilesonly", QUERY_FLAG_FILES_ONLY, REMOVE_FLAG},
    {"folder", QUERY_FLAG_FOLDERS_ONLY, ADD_FLAG},
    {"nofolderonly", QUERY_FLAG_FOLDERS_ONLY, REMOVE_FLAG},
    {"folders", QUERY_FLAG_FOLDERS_ONLY, ADD_FLAG},
    {"nofoldersonly", QUERY_FLAG_FOLDERS_ONLY, REMOVE_FLAG},
    {"path", QUERY_FLAG_SEARCH_IN_PATH, ADD_FLAG},
    {"nopath", QUERY_FLAG_SEARCH_IN_PATH, REMOVE_FLAG},
    {"regex", QUERY_FLAG_REGEX, ADD_FLAG},
    {"noregex", QUERY_FLAG_REGEX, REMOVE_FLAG},
};

FsearchTokenFunction supported_functions[] = {
    {"childcount", parse_function_childcount},
    {"childfilecount", parse_function_childfilecount},
    {"childfoldercount", parse_function_childfoldercount},
    {"contenttype", parse_function_contenttype},
    {"depth", parse_function_depth},
    {"dm", parse_function_date_modified},
    {"datemodified", parse_function_date_modified},
    {"empty", parse_function_empty},
    {"ext", parse_function_extension},
    {"parent", parse_function_parent},
    {"parents", parse_function_depth},
    {"size", parse_function_size},
};

static GList *
new_list(void *element) {
    return element ? g_list_append(NULL, element) : NULL;
}

static GList *
append_to_list_if_nonnull(GList *list, void *element) {
    return element ? g_list_append(list, element) : list;
}

static bool
expect_word(FsearchQueryLexer *lexer, GString **string) {
    if (fsearch_query_lexer_get_next_token(lexer, string) == FSEARCH_QUERY_TOKEN_WORD) {
        return true;
    }
    return false;
}

static FsearchQueryNode *
parse_numeric_field_with_optional_range(const char *field_name,
                                        FsearchQueryIntegerParserFunc parse_value_func,
                                        FsearchQueryComparisonNewNodeFunc new_node_func,
                                        GString *string,
                                        FsearchQueryFlags flags) {
    g_auto(GStrv) elements = g_strsplit(string->str, "..", 2);
    if (!elements || !elements[0]) {
        goto fail;
    }

    int64_t start = 0;
    int64_t end = 0;
    FsearchQueryNodeComparison comp_type = FSEARCH_QUERY_NODE_COMPARISON_EQUAL;

    if (fsearch_string_is_empty(elements[0])) {
        // query starts with ..
        // e.g. dm:..january
        start = 0;
        comp_type = FSEARCH_QUERY_NODE_COMPARISON_RANGE;
    }
    else {
        if (parse_value_func(elements[0], &start, &end)) {
            comp_type = FSEARCH_QUERY_NODE_COMPARISON_RANGE;
        }
        else {
            goto fail;
        }
    }

    if (elements[1]) {
        if (fsearch_string_is_empty(elements[1])) {
            // query ends with ..
            // e.g. dm:january..
            end = INT32_MAX;
            comp_type = FSEARCH_QUERY_NODE_COMPARISON_GREATER_EQ;
        }
        else {
            if (parse_value_func(elements[1], NULL, &end)) {
                comp_type = FSEARCH_QUERY_NODE_COMPARISON_RANGE;
            }
            else {
                goto fail;
            }
        }
    }

    if (start == end) {
        comp_type = FSEARCH_QUERY_NODE_COMPARISON_EQUAL;
    }

    return new_node_func(flags, start, end, comp_type);

fail:
    g_debug("[%s:] invalid argument: %s", field_name, string->str);
    return fsearch_query_node_new_match_nothing();
}

static bool
parse_integer(const char *str, int64_t *num_out, int64_t *num_2_out) {
    char *num_suffix = NULL;
    int64_t num = strtoll(str, &num_suffix, 10);
    if (num_suffix == str) {
        // Failed to parse a number
        return false;
    }
    if (num_suffix && num_suffix[0] != '\0') {
        // We don't accept numbers followed by non-digit characters
        return false;
    }
    if (num_out) {
        *num_out = num;
    }
    if (num_2_out) {
        *num_2_out = num;
    }
    return true;
}

static GList *
parse_numeric_function(FsearchQueryParseContext *parse_ctx,
                       bool is_empty_field,
                       FsearchQueryFlags flags,
                       const char *field_name,
                       FsearchQueryComparisonNewNodeFunc new_node_func,
                       FsearchQueryIntegerParserFunc parse_value_func) {
    if (is_empty_field) {
        // we want to match every entry for an empty numeric field
        return new_list(fsearch_query_node_new_match_everything(flags));
    }
    g_autoptr(GString) token_value = NULL;
    FsearchQueryToken token = fsearch_query_lexer_get_next_token(parse_ctx->lexer, &token_value);
    FsearchQueryNodeComparison comp_type = FSEARCH_QUERY_NODE_COMPARISON_EQUAL;
    switch (token) {
    case FSEARCH_QUERY_TOKEN_EQUAL:
        comp_type = FSEARCH_QUERY_NODE_COMPARISON_EQUAL;
        break;
    case FSEARCH_QUERY_TOKEN_SMALLER:
        comp_type = FSEARCH_QUERY_NODE_COMPARISON_SMALLER;
        break;
    case FSEARCH_QUERY_TOKEN_SMALLER_EQ:
        comp_type = FSEARCH_QUERY_NODE_COMPARISON_SMALLER_EQ;
        break;
    case FSEARCH_QUERY_TOKEN_GREATER:
        comp_type = FSEARCH_QUERY_NODE_COMPARISON_GREATER;
        break;
    case FSEARCH_QUERY_TOKEN_GREATER_EQ:
        comp_type = FSEARCH_QUERY_NODE_COMPARISON_GREATER_EQ;
        break;
    case FSEARCH_QUERY_TOKEN_WORD:
        // query has the form of field:<val> or field:<opt_val_1>..<opt_val_2>
        return new_list(
            parse_numeric_field_with_optional_range(field_name, parse_value_func, new_node_func, token_value, flags));
    default:
        g_debug("[%s:] invalid or missing argument", field_name);
        return new_list(fsearch_query_node_new_match_nothing());
    }

    g_autoptr(GString) next_token_value = NULL;
    if (expect_word(parse_ctx->lexer, &next_token_value)) {
        int64_t val_1 = 0;
        int64_t val_2 = 0;
        if (parse_value_func(next_token_value->str, &val_1, &val_2)) {
            return new_list(new_node_func(flags, val_1, val_2, comp_type));
        }
    }

    return new_list(fsearch_query_node_new_match_nothing());
}

static GList *
parse_function_size(FsearchQueryParseContext *parse_ctx, bool is_empty_field, FsearchQueryFlags flags) {
    return parse_numeric_function(parse_ctx, is_empty_field, flags, "size", fsearch_query_node_new_size, fsearch_size_parse);
}

static GList *
parse_function_empty(FsearchQueryParseContext *parse_ctx, bool is_empty_field, FsearchQueryFlags flags) {
    return new_list(fsearch_query_node_new_childcount(flags, 0, 0, FSEARCH_QUERY_NODE_COMPARISON_EQUAL));
}

static GList *
parse_function_depth(FsearchQueryParseContext *parse_ctx, bool is_empty_field, FsearchQueryFlags flags) {
    return parse_numeric_function(parse_ctx, is_empty_field, flags, "depth", fsearch_query_node_new_depth, parse_integer);
}

static GList *
parse_function_childcount(FsearchQueryParseContext *parse_ctx, bool is_empty_field, FsearchQueryFlags flags) {
    return parse_numeric_function(parse_ctx,
                                  is_empty_field,
                                  flags,
                                  "childcount",
                                  fsearch_query_node_new_childcount,
                                  parse_integer);
}

static GList *
parse_function_childfilecount(FsearchQueryParseContext *parse_ctx, bool is_empty_field, FsearchQueryFlags flags) {
    return parse_numeric_function(parse_ctx,
                                  is_empty_field,
                                  flags,
                                  "childfilecount",
                                  fsearch_query_node_new_childfilecount,
                                  parse_integer);
}

static GList *
parse_function_childfoldercount(FsearchQueryParseContext *parse_ctx, bool is_empty_field, FsearchQueryFlags flags) {
    return parse_numeric_function(parse_ctx,
                                  is_empty_field,
                                  flags,
                                  "childfoldercount",
                                  fsearch_query_node_new_childfoldercount,
                                  parse_integer);
}

static GList *
parse_function_date_modified(FsearchQueryParseContext *parse_ctx, bool is_empty_field, FsearchQueryFlags flags) {
    return parse_numeric_function(parse_ctx,
                                  is_empty_field,
                                  flags,
                                  "date-modified",
                                  fsearch_query_node_new_date_modified,
                                  (FsearchQueryIntegerParserFunc *)fsearch_date_time_parse_interval);
}

static GList *
parse_function_extension(FsearchQueryParseContext *parse_ctx, bool is_empty_field, FsearchQueryFlags flags) {
    if (is_empty_field) {
        // an empty field means that we want to match all files which lack file extensions
        return new_list(fsearch_query_node_new_extension(NULL, flags));
    }

    g_autoptr(GString) token_value = NULL;
    if (expect_word(parse_ctx->lexer, &token_value)) {
        return new_list(fsearch_query_node_new_extension(token_value->str, flags));
    }
    return new_list(fsearch_query_node_new_match_nothing());
}

static GList *
parse_function_contenttype(FsearchQueryParseContext *parse_ctx, bool is_empty_field, FsearchQueryFlags flags) {
    if (is_empty_field) {
        return new_list(fsearch_query_node_new_match_everything(flags));
    }

    g_autoptr(GString) token_value = NULL;
    if (expect_word(parse_ctx->lexer, &token_value)) {
        return new_list(fsearch_query_node_new_contenttype(token_value->str, flags));
    }
    return new_list(fsearch_query_node_new_match_nothing());
}

static GList *
parse_function_parent(FsearchQueryParseContext *parse_ctx, bool is_empty_field, FsearchQueryFlags flags) {
    FsearchQueryFlags parent_flags = flags | QUERY_FLAG_EXACT_MATCH;
    if (is_empty_field) {
        // an empty field means that we want to match all entries without a parent
        return new_list(fsearch_query_node_new_parent("", parent_flags));
    }

    g_autoptr(GString) token_value = NULL;
    if (expect_word(parse_ctx->lexer, &token_value)) {
        return new_list(fsearch_query_node_new_parent(token_value->str, parent_flags));
    }
    return new_list(fsearch_query_node_new_match_nothing());
}

static GList *
parse_modifier(FsearchQueryParseContext *parse_ctx, bool is_empty_field, FsearchQueryFlags flags) {
    if (is_empty_field) {
        return new_list(fsearch_query_node_new_match_everything(flags));
    }
    g_autoptr(GString) token_value = NULL;
    FsearchQueryToken token = fsearch_query_lexer_get_next_token(parse_ctx->lexer, &token_value);
    if (token == FSEARCH_QUERY_TOKEN_WORD) {
        return parse_word(token_value, flags);
    }
    else if (token == FSEARCH_QUERY_TOKEN_BRACKET_OPEN) {
        GList *res = parse_open_bracket(parse_ctx);
        return g_list_concat(res, fsearch_query_parser_parse_expression(parse_ctx, true, flags));
    }
    else if (token == FSEARCH_QUERY_TOKEN_FIELD) {
        return parse_field(parse_ctx, token_value, false, flags);
    }
    else if (token == FSEARCH_QUERY_TOKEN_FIELD_EMPTY) {
        return parse_field(parse_ctx, token_value, true, flags);
    }
    return new_list(fsearch_query_node_new_match_nothing());
}

static GList *
parse_filter_macros(FsearchQueryParseContext *parse_ctx, GString *name, FsearchQueryFlags flags) {
    GList *res = NULL;
    for (uint32_t i = 0; i < parse_ctx->macro_filters->len; ++i) {
        FsearchFilter *filter = g_ptr_array_index(parse_ctx->macro_filters, i);
        if (strcmp(name->str, filter->macro) != 0) {
            continue;
        }

        // Make sure that macros don't call themselves, which would end up in an endless loop
        if (g_queue_find(parse_ctx->macro_stack, filter)) {
            g_debug("[expand_filter_macros] nested macro detected. Stop parsing of macro.");
            break;
        }
        if (fsearch_string_is_empty(filter->query)) {
            // We don't need to process an empty macro query
            break;
        }

        // Apply the filter flags
        if (filter->flags & QUERY_FLAG_SEARCH_IN_PATH) {
            flags |= QUERY_FLAG_SEARCH_IN_PATH;
        }
        if (filter->flags & QUERY_FLAG_MATCH_CASE) {
            flags |= QUERY_FLAG_MATCH_CASE;
        }
        if (filter->flags & QUERY_FLAG_REGEX) {
            flags |= QUERY_FLAG_REGEX;
        }

        g_queue_push_tail(parse_ctx->macro_stack, filter);

        // The process of parsing macros gets its own lexer and operator stack
        // to make sure that they're not affected by the current parsing state
        // and vice versa
        GQueue *main_operator_stack = parse_ctx->operator_stack;
        FsearchQueryLexer *main_lexer = parse_ctx->lexer;

        parse_ctx->lexer = fsearch_query_lexer_new(filter->query);
        parse_ctx->operator_stack = g_queue_new();
        res = fsearch_query_parser_parse_expression(parse_ctx, false, flags);
        if (!g_queue_is_empty(parse_ctx->operator_stack)) {
            g_warning("[parse_macro] operator stack not empty after parsing!\n");
        }
        g_clear_pointer(&parse_ctx->lexer, fsearch_query_lexer_free);
        g_clear_pointer(&parse_ctx->operator_stack, g_queue_free);

        parse_ctx->operator_stack = main_operator_stack;
        parse_ctx->lexer = main_lexer;
        g_queue_pop_tail(parse_ctx->macro_stack);

        break;
    }
    return res;
}

static GList *
parse_field(FsearchQueryParseContext *parse_ctx, GString *field_name, bool is_empty_field, FsearchQueryFlags flags) {
    // Macros have precedence over native fields
    GList *res = parse_filter_macros(parse_ctx, field_name, flags);
    if (!res) {
        for (uint32_t i = 0; i < G_N_ELEMENTS(supported_modifiers); ++i) {
            FsearchTokenModifier *modifier = &supported_modifiers[i];
            if (!strcmp(modifier->name, field_name->str)) {
                if (modifier->flag_operation == ADD_FLAG) {
                    flags |= modifier->flag;
                }
                else {
                    flags &= ~modifier->flag;
                }
                return parse_modifier(parse_ctx, is_empty_field, flags);
            }
        }
        for (uint32_t i = 0; i < G_N_ELEMENTS(supported_functions); ++i) {
            FsearchTokenFunction *function = &supported_functions[i];
            if (!strcmp(function->name, field_name->str)) {
                return function->parser(parse_ctx, is_empty_field, flags);
            }
        }
    }
    return res ? res : new_list(fsearch_query_node_new_match_nothing());
}

static GList *
parse_word(GString *field_name, FsearchQueryFlags flags) {
    if (!field_name) {
        return NULL;
    }
    return new_list(fsearch_query_node_new(field_name->str, flags));
}

static FsearchQueryToken
top_query_token(GQueue *stack) {
    if (g_queue_is_empty(stack)) {
        return FSEARCH_QUERY_TOKEN_NONE;
    }
    return (FsearchQueryToken)GPOINTER_TO_UINT(g_queue_peek_tail(stack));
}

static FsearchQueryToken
pop_query_token(GQueue *stack) {
    if (g_queue_is_empty(stack)) {
        return FSEARCH_QUERY_TOKEN_NONE;
    }
    return (FsearchQueryToken)(GPOINTER_TO_UINT(g_queue_pop_tail(stack)));
}

static void
push_query_token(GQueue *stack, FsearchQueryToken token) {
    g_queue_push_tail(stack, GUINT_TO_POINTER((guint)token));
}

static uint32_t
get_operator_precedence(FsearchQueryToken operator) {
    switch (operator) {
    case FSEARCH_QUERY_TOKEN_NOT:
        return 3;
    case FSEARCH_QUERY_TOKEN_AND:
        return 2;
    case FSEARCH_QUERY_TOKEN_OR:
        return 1;
    default:
        return 0;
    }
}

static FsearchQueryNode *
get_operator_node_for_query_token(FsearchQueryToken token) {
    FsearchQueryNodeOperator op = 0;
    switch (token) {
    case FSEARCH_QUERY_TOKEN_AND:
        op = FSEARCH_QUERY_NODE_OPERATOR_AND;
        break;
    case FSEARCH_QUERY_TOKEN_OR:
        op = FSEARCH_QUERY_NODE_OPERATOR_OR;
        break;
    case FSEARCH_QUERY_TOKEN_NOT:
        op = FSEARCH_QUERY_NODE_OPERATOR_NOT;
        break;
    default:
        return NULL;
    }
    return fsearch_query_node_new_operator(op);
}

static bool
is_operand_token(FsearchQueryToken token) {
    switch (token) {
    case FSEARCH_QUERY_TOKEN_WORD:
    case FSEARCH_QUERY_TOKEN_FIELD:
    case FSEARCH_QUERY_TOKEN_FIELD_EMPTY:
        return true;
    default:
        return false;
    }
}

static GList *
get_implicit_and_if_necessary(FsearchQueryParseContext *parse_ctx,
                              FsearchQueryToken last_token,
                              FsearchQueryToken next_token) {
    // Before an implicit and there must either be an operand or a closing bracket
    if (is_operand_token(last_token) || last_token == FSEARCH_QUERY_TOKEN_BRACKET_CLOSE) {
        // After an implicit AND there must either be an operand or an opening bracket or a NOT operator
        if (is_operand_token(next_token) || next_token == FSEARCH_QUERY_TOKEN_BRACKET_OPEN
            || next_token == FSEARCH_QUERY_TOKEN_NOT) {
            return parse_operator(parse_ctx, FSEARCH_QUERY_TOKEN_AND);
        }
    }
    return NULL;
}

static bool
is_binary_operator_token(FsearchQueryToken token) {
    if (token == FSEARCH_QUERY_TOKEN_AND || token == FSEARCH_QUERY_TOKEN_OR) {
        return true;
    }
    return false;
}

static bool
is_operator_token(FsearchQueryToken token) {
    if (is_binary_operator_token(token) || token == FSEARCH_QUERY_TOKEN_NOT) {
        return true;
    }
    return false;
}

static bool
is_operator_token_followed_by_operand(FsearchQueryLexer *lexer, FsearchQueryToken op_token) {
    g_assert(is_operator_token(op_token));

    FsearchQueryToken next_token = fsearch_query_lexer_peek_next_token(lexer, NULL);
    // FIXME: while a NOT operator usually is followed directly by an operand this assumption might cause some bugs
    if (is_binary_operator_token(op_token) && next_token == FSEARCH_QUERY_TOKEN_NOT) {
        return true;
    }
    return is_operand_token(next_token) || next_token == FSEARCH_QUERY_TOKEN_BRACKET_OPEN;
}

static GList *
parse_operator(FsearchQueryParseContext *parse_ctx, FsearchQueryToken token) {
    g_assert(is_operator_token(token));

    parse_ctx->last_token = token;
    GList *res = NULL;
    // Before we can add the operator token to the operator stack we first have to pop and handle all operators on the
    // stack until we find the first one with a lower precedence than the one we're about to add
    while (!g_queue_is_empty(parse_ctx->operator_stack)
           && get_operator_precedence(token) <= get_operator_precedence(top_query_token(parse_ctx->operator_stack))) {
        res = append_to_list_if_nonnull(res,
                                        get_operator_node_for_query_token(pop_query_token(parse_ctx->operator_stack)));
    }
    push_query_token(parse_ctx->operator_stack, token);
    return res;
}

static bool
consume_consecutive_not_token(FsearchQueryLexer *lexer) {
    bool uneven_number_of_not_tokens = true;
    while (fsearch_query_lexer_peek_next_token(lexer, NULL) == FSEARCH_QUERY_TOKEN_NOT) {
        fsearch_query_lexer_get_next_token(lexer, NULL);
        uneven_number_of_not_tokens = !uneven_number_of_not_tokens;
    }
    return uneven_number_of_not_tokens;
}

static void
discard_consecutive_binary_operator_tokens(FsearchQueryLexer *lexer) {
    // discard all consecutive AND and OR operators until we find a different token
    while (is_binary_operator_token(fsearch_query_lexer_peek_next_token(lexer, NULL))) {
        fsearch_query_lexer_get_next_token(lexer, NULL);
    }
}

static GList *
parse_close_bracket(FsearchQueryParseContext *parse_ctx) {
    GList *res = NULL;
    // pop and handle all operators from the stack until we either find our matching open bracket
    // or reach the end of the stack
    while (true) {
        FsearchQueryToken t = top_query_token(parse_ctx->operator_stack);
        if (t == FSEARCH_QUERY_TOKEN_BRACKET_OPEN) {
            // Found first and matching open bracket. Done.
            pop_query_token(parse_ctx->operator_stack);
            break;
        }
        if (t == FSEARCH_QUERY_TOKEN_NONE) {
            // There hasn't been an open bracket on the stack. This must not happen, because we earlier made sure
            // that closing brackets will only be handled if there are at least as much open as closing brackets.
            g_warning("[infix-postfix] Matching open bracket not found!\n");
            g_assert_not_reached();
        }
        // AND, OR, NOT operator found, append them to the result list
        res = append_to_list_if_nonnull(res,
                                        get_operator_node_for_query_token(pop_query_token(parse_ctx->operator_stack)));
    }

    parse_ctx->last_token = FSEARCH_QUERY_TOKEN_BRACKET_CLOSE;
    return res;
}

static GList *
parse_open_bracket(FsearchQueryParseContext *parse_ctx) {
    GList *res = get_implicit_and_if_necessary(parse_ctx, parse_ctx->last_token, FSEARCH_QUERY_TOKEN_BRACKET_OPEN);
    parse_ctx->last_token = FSEARCH_QUERY_TOKEN_BRACKET_OPEN;
    push_query_token(parse_ctx->operator_stack, FSEARCH_QUERY_TOKEN_BRACKET_OPEN);
    return res;
}

GList *
fsearch_query_parser_parse_expression(FsearchQueryParseContext *parse_ctx, bool in_open_bracket, FsearchQueryFlags flags) {
    GList *res = NULL;

    uint32_t num_open_brackets = in_open_bracket ? 1 : 0;
    uint32_t num_close_brackets = 0;

    while (true) {
        g_autoptr(GString) token_value = NULL;
        FsearchQueryToken token = fsearch_query_lexer_get_next_token(parse_ctx->lexer, &token_value);
        FsearchQueryToken last_token = parse_ctx->last_token;

        // When a NOT operator is parsed, an implicit AND is added in the process (if necessary)
        // so in that case an additional implicit AND check before adding the parsed result to the final list
        // isn't necessary anymore and can be skipped
        bool skip_implicit_and_check = false;

        GList *to_append = NULL;
        switch (token) {
        case FSEARCH_QUERY_TOKEN_EOS:
            goto out;
        case FSEARCH_QUERY_TOKEN_NOT:
            if (consume_consecutive_not_token(parse_ctx->lexer)) {
                // We want to support consecutive NOT operators (i.e. `NOT NOT a`)
                // so even numbers of NOT operators get ignored and for uneven numbers we add a single one only
                if (is_operator_token_followed_by_operand(parse_ctx->lexer, token)) {
                    skip_implicit_and_check = true;
                    to_append = get_implicit_and_if_necessary(parse_ctx, last_token, token);
                    to_append = g_list_concat(to_append, parse_operator(parse_ctx, token));
                }
            }
            break;
        case FSEARCH_QUERY_TOKEN_AND:
        case FSEARCH_QUERY_TOKEN_OR:
            // only add an operator if it's followed by an operand
            if (is_operator_token_followed_by_operand(parse_ctx->lexer, token)) {
                to_append = parse_operator(parse_ctx, token);
            }
            break;
        case FSEARCH_QUERY_TOKEN_BRACKET_OPEN:
            num_open_brackets++;
            to_append = parse_open_bracket(parse_ctx);
            // We discard any operator tokens directly after an open bracket (excluding NOT operators),
            // because there's no left-hand side operand they could act on.
            // It's safe to assume that queries like `( OR abc OR efg)` should be interpreted as `(abc OR efg)` anyway
            discard_consecutive_binary_operator_tokens(parse_ctx->lexer);
            break;
        case FSEARCH_QUERY_TOKEN_BRACKET_CLOSE:
            // only add closing bracket if there's a matching open bracket
            if (num_open_brackets > num_close_brackets) {
                num_close_brackets++;
                to_append = parse_close_bracket(parse_ctx);

                if (in_open_bracket && num_close_brackets == num_open_brackets) {
                    // We found the matching closing bracket which marks the end of this expression, return.
                    if (to_append) {
                        return g_list_concat(res, to_append);
                    }
                    else {
                        return res;
                    }
                }
            }
            else {
                g_debug("[infix-postfix] closing bracket found without a corresponding open bracket, abort "
                        "parsing!\n");
                g_list_free_full(g_steal_pointer(&res), (GDestroyNotify)fsearch_query_node_free);
                return new_list(fsearch_query_node_new_match_nothing());
            }
            break;
        case FSEARCH_QUERY_TOKEN_WORD:
            to_append = parse_word(token_value, flags);
            break;
        case FSEARCH_QUERY_TOKEN_FIELD:
            to_append = parse_field(parse_ctx, token_value, false, flags);
            break;
        case FSEARCH_QUERY_TOKEN_FIELD_EMPTY:
            to_append = parse_field(parse_ctx, token_value, true, flags);
            break;
        default:
            g_debug("[infix-postfix] ignoring unexpected token: %d", token);
            break;
        }

        if (to_append) {
            if (!skip_implicit_and_check) {
                res = g_list_concat(res, get_implicit_and_if_necessary(parse_ctx, last_token, token));
            }
            parse_ctx->last_token = token;
            res = g_list_concat(res, to_append);
        }
    }

out:
    while (!g_queue_is_empty(parse_ctx->operator_stack)) {
        res = append_to_list_if_nonnull(res,
                                        get_operator_node_for_query_token(pop_query_token(parse_ctx->operator_stack)));
    }
    return res;
}
