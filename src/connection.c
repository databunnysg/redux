#include "connection.h"
#include "conversions.h"

static void redis_finalize(SEXP extPtr);

// API functions first:
SEXP redux_redis_connect(SEXP host, SEXP port) {
  redisContext *context = redisConnect(CHAR(STRING_ELT(host, 0)),
                                       INTEGER(port)[0]);
  if (context == NULL) {
    error("Creating context failed catastrophically");
  }
  if (context->err != 0) {
    error("Failed to create context: %s", context->errstr);
  }
  SEXP extPtr = PROTECT(R_MakeExternalPtr(context, host, R_NilValue));
  R_RegisterCFinalizer(extPtr, redis_finalize);
  UNPROTECT(1);
  return extPtr;
}

SEXP redux_redis_connect_unix(SEXP path) {
  redisContext *context = redisConnectUnix(CHAR(STRING_ELT(path, 0)));
  if (context == NULL) {
    error("Creating context failed catastrophically");
  }
  if (context->err != 0) {
    error("Failed to create context: %s", context->errstr);
  }
  SEXP extPtr = PROTECT(R_MakeExternalPtr(context, path, R_NilValue));
  R_RegisterCFinalizer(extPtr, redis_finalize);
  UNPROTECT(1);
  return extPtr;
}

SEXP redux_redis_command(SEXP extPtr, SEXP cmd) {
  redisContext *context = redis_get_context(extPtr, CLOSED_ERROR);

  cmd = PROTECT(redis_check_command(cmd));
  const char **argv = NULL;
  size_t *argvlen = NULL;
  const size_t argc = sexp_to_redis(cmd, &argv, &argvlen);

  redisReply *reply = redisCommandArgv(context, argc, argv, argvlen);
  SEXP ret = PROTECT(redis_reply_to_sexp(reply, REPLY_ERROR_THROW));
  freeReplyObject(reply);
  UNPROTECT(2);
  return ret;
}

// I don't think that append/get reply are safe from R because it's
// too easy to lock the process up.  So focus instead on a "pipline"
// operation that has some reasonable guarantees about R errors.
SEXP redux_redis_pipeline(SEXP extPtr, SEXP list) {
  if (TYPEOF(list) != VECSXP) {
    error("Expected list character argument");
  }
  redisContext *context = redis_get_context(extPtr, CLOSED_ERROR);

  // Now, try and do the basic processing of *all* commands before
  // sending any.
  list = PROTECT(redis_check_list(list));
  const size_t nc = LENGTH(list);
  const char ***argv = (const char***) R_alloc(nc, sizeof(const char**));
  size_t **argvlen = (size_t**) R_alloc(nc, sizeof(size_t*));
  size_t *argc = (size_t*) R_alloc(nc, sizeof(size_t));
  for (size_t i = 0; i < nc; ++i) {
    argc[i] = sexp_to_redis(VECTOR_ELT(list, i), argv + i, argvlen + i);
  }

  for (size_t i = 0; i < nc; ++i) {
    redisAppendCommandArgv(context, argc[i], argv[i], argvlen[i]);
  }

  redisReply *reply = NULL;
  SEXP ret = PROTECT(allocVector(VECSXP, nc));
  for (size_t i = 0; i < nc; ++i) {
    redisGetReply(context, (void*)&reply);
    SET_VECTOR_ELT(ret, i, redis_reply_to_sexp(reply, REPLY_ERROR_OK));
    freeReplyObject(reply);
  }
  UNPROTECT(2);
  return ret;
}

// Internal functions:
redisContext* redis_get_context(SEXP extPtr, int closed_action) {
  // It is not possible here to be *generally* typesafe, short of
  // adding (and checking at every command) that we have an external
  // pointer to the correct type.  So cross fingers and hope for the
  // best which is what most packages do I believe.  We can, however,
  // check that we're getting a pointer from the correct sort of
  // thing, and that the pointer is not NULL.
  void *context = NULL;
  if (TYPEOF(extPtr) != EXTPTRSXP) {
    error("Expected an external pointer");
  }
  context = (redisContext*) R_ExternalPtrAddr(extPtr);
  if (!context) {
    if (closed_action == CLOSED_WARN) {
      warning("Context is not connected");
    } else if (closed_action == CLOSED_ERROR) {
      error("Context is not connected");
    }
  }
  return context;
}

static void redis_finalize(SEXP extPtr) {
  redisContext *context = redis_get_context(extPtr, CLOSED_PASS);
  if (context) {
    redisFree(context);
  }
}
