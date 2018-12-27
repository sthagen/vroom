#include "altrep.h"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-compare"
#include <mio/shared_mmap.hpp>
#pragma clang diagnostic pop

#include "parallel.h"
#include "readidx_vec.h"

using namespace Rcpp;

// inspired by Luke Tierney and the R Core Team
// https://github.com/ALTREP-examples/Rpkg-mutable/blob/master/src/mutable.c
// and Romain François
// https://purrple.cat/blog/2018/10/21/lazy-abs-altrep-cplusplus/ and Dirk

template <class TYPE> class readidx_numeric : public readidx_vec {

public:
  static R_altrep_class_t class_t;

  // Make an altrep object of class `stdvec_double::class_t`
  static SEXP Make(
      std::shared_ptr<std::vector<size_t> >* offsets,
      mio::shared_mmap_source* mmap,
      R_xlen_t column,
      R_xlen_t num_columns,
      R_xlen_t skip,
      R_xlen_t num_threads) {

    // `out` and `xp` needs protection because R_new_altrep allocates
    SEXP out = PROTECT(Rf_allocVector(VECSXP, 6));

    SEXP idx_xp = PROTECT(R_MakeExternalPtr(offsets, R_NilValue, R_NilValue));
    R_RegisterCFinalizerEx(idx_xp, Finalize_Idx, TRUE);

    SEXP mmap_xp = PROTECT(R_MakeExternalPtr(mmap, R_NilValue, R_NilValue));
    R_RegisterCFinalizerEx(mmap_xp, Finalize_Mmap, TRUE);

    SET_VECTOR_ELT(out, 0, idx_xp);
    SET_VECTOR_ELT(out, 1, mmap_xp);
    SET_VECTOR_ELT(out, 2, Rf_ScalarReal(column));
    SET_VECTOR_ELT(out, 3, Rf_ScalarReal(num_columns));
    SET_VECTOR_ELT(out, 4, Rf_ScalarReal(skip));
    SET_VECTOR_ELT(out, 5, Rf_ScalarReal(num_threads));

    // make a new altrep object of class `readidx_real::class_t`
    SEXP res = R_new_altrep(class_t, out, R_NilValue);

    UNPROTECT(3);

    return res;
  }

  // ALTREP methods -------------------

  // What gets printed when .Internal(inspect()) is used
  static Rboolean Inspect(
      SEXP x,
      int pre,
      int deep,
      int pvec,
      void (*inspect_subtree)(SEXP, int, int, int)) {
    Rprintf(
        "readidx_numeric (len=%d, materialized=%s)\n",
        Length(x),
        R_altrep_data2(x) != R_NilValue ? "T" : "F");
    return TRUE;
  }

  // ALTREAL methods -----------------

  // --- Altvec
  static SEXP Materialize(SEXP vec) {
    SEXP data2 = R_altrep_data2(vec);
    if (data2 != R_NilValue) {
      return data2;
    }

    // allocate a standard numeric vector for data2
    R_xlen_t n = Length(vec);
    TYPE out(n);

    auto p = out.begin();

    auto sep_locs = Idx(vec);
    auto column = Column(vec);
    auto num_columns = Num_Columns(vec);
    auto skip = Skip(vec);

    mio::shared_mmap_source* mmap = Mmap(vec);

    // Need to copy to a temp buffer since we have no way to tell strtod how
    // long the buffer is.
    char buf[128];

    parallel_for(
        n,
        [&](int start, int end, int id) {
          for (int i = start; i < end; ++i) {
            size_t idx = (i + skip) * num_columns + column;
            size_t cur_loc = (*sep_locs)[idx];
            size_t next_loc = (*sep_locs)[idx + 1] - 1;
            size_t len = next_loc - cur_loc;

            std::copy(mmap->data() + cur_loc, mmap->data() + next_loc, buf);
            buf[len] = '\0';

            p[i] = R_strtod(buf, NULL);
          }
        },
        Num_Threads(vec));

    R_set_altrep_data2(vec, out);

    return out;
  }

  // Fills buf with contents from the i item
  static void buf_Elt(SEXP vec, size_t i, char* buf) {
    auto sep_locs = Idx(vec);
    auto column = Column(vec);
    auto num_columns = Num_Columns(vec);
    auto skip = Skip(vec);

    size_t idx = (i + skip) * num_columns + column;
    size_t cur_loc = (*sep_locs)[idx];
    size_t next_loc = (*sep_locs)[idx + 1] - 1;
    size_t len = next_loc - cur_loc;
    // Rcerr << cur_loc << ':' << next_loc << ':' << len << '\n';

    mio::shared_mmap_source* mmap = Mmap(vec);

    // Need to copy to a temp buffer since we have no way to tell strtod how
    // long the buffer is.
    std::copy(mmap->data() + cur_loc, mmap->data() + next_loc, buf);
    buf[len] = '\0';
  }

  static void* Dataptr(SEXP vec, Rboolean writeable) {
    return STDVEC_DATAPTR(Materialize(vec));
  }

  // -------- initialize the altrep class with the methods above
};

typedef readidx_numeric<NumericVector> readidx_real;

template <> R_altrep_class_t readidx_real::class_t{};

// the element at the index `i`
double real_Elt(SEXP vec, R_xlen_t i) {
  SEXP data2 = R_altrep_data2(vec);
  if (data2 != R_NilValue) {
    return REAL(data2)[i];
  }
  char buf[128];
  readidx_real::buf_Elt(vec, i, buf);

  return R_strtod(buf, NULL);
}

// Called the package is loaded (needs Rcpp 0.12.18.3)
// [[Rcpp::init]]
void init_readidx_real(DllInfo* dll) {
  readidx_real::class_t = R_make_altreal_class("readidx_real", "readidx", dll);

  // altrep
  R_set_altrep_Length_method(readidx_real::class_t, readidx_real::Length);
  R_set_altrep_Inspect_method(readidx_real::class_t, readidx_real::Inspect);

  // altvec
  R_set_altvec_Dataptr_method(readidx_real::class_t, readidx_real::Dataptr);
  R_set_altvec_Dataptr_or_null_method(
      readidx_real::class_t, readidx_real::Dataptr_or_null);

  // altreal
  R_set_altreal_Elt_method(readidx_real::class_t, real_Elt);
}

typedef readidx_numeric<IntegerVector> readidx_int;

template <> R_altrep_class_t readidx_int::class_t{};

// https://github.com/wch/r-source/blob/efed16c945b6e31f8e345d2f18e39a014d2a57ae/src/main/scan.c#L145-L157
static int Strtoi(const char* nptr, int base) {
  long res;
  char* endp;

  errno = 0;
  res = strtol(nptr, &endp, base);
  if (*endp != '\0')
    res = NA_INTEGER;
  /* next can happen on a 64-bit platform */
  if (res > INT_MAX || res < INT_MIN)
    res = NA_INTEGER;
  if (errno == ERANGE)
    res = NA_INTEGER;
  return (int)res;
}

// the element at the index `i`
int int_Elt(SEXP vec, R_xlen_t i) {
  SEXP data2 = R_altrep_data2(vec);
  if (data2 != R_NilValue) {
    return INTEGER(data2)[i];
  }
  char buf[128];
  readidx_int::buf_Elt(vec, i, buf);

  return Strtoi(buf, 10);
}

// Called the package is loaded (needs Rcpp 0.12.18.3)
// [[Rcpp::init]]
void init_readidx_int(DllInfo* dll) {
  readidx_int::class_t = R_make_altinteger_class("readidx_int", "readidx", dll);

  // altrep
  R_set_altrep_Length_method(readidx_int::class_t, readidx_int::Length);
  R_set_altrep_Inspect_method(readidx_int::class_t, readidx_int::Inspect);

  // altvec
  R_set_altvec_Dataptr_method(readidx_int::class_t, readidx_int::Dataptr);
  R_set_altvec_Dataptr_or_null_method(
      readidx_int::class_t, readidx_int::Dataptr_or_null);

  // altinteger
  R_set_altinteger_Elt_method(readidx_int::class_t, int_Elt);
}

// Altrep for Logical vectors does not yet exist
// typedef readidx_numeric<LogicalVector> readidx_lgl;

// template <> R_altrep_class_t readidx_lgl::class_t{};

//// the element at the index `i`
// int lgl_Elt(SEXP vec, R_xlen_t i) {
// SEXP data2 = R_altrep_data2(vec);
// if (data2 != R_NilValue) {
// return INTEGER(data2)[i];
//}
// char buf[128];
// readidx_lgl::buf_Elt(vec, i, buf);

// return Rf_StringTrue(buf);
//}

//// Called the package is loaded (needs Rcpp 0.12.18.3)
//// [[Rcpp::init]]
// void init_readidx_lgl(DllInfo* dll) {
// readidx_lgl::class_t = R_make_altinteger_class("readidx_lgl", "readidx",
// dll);

//// altrep
// R_set_altrep_Length_method(readidx_lgl::class_t, readidx_lgl::Length);
// R_set_altrep_Inspect_method(readidx_lgl::class_t, readidx_lgl::Inspect);

//// altvec
// R_set_altvec_Dataptr_method(readidx_lgl::class_t, readidx_lgl::Dataptr);
// R_set_altvec_Dataptr_or_null_method(
// readidx_lgl::class_t, readidx_lgl::Dataptr_or_null);

//// altinteger
// R_set_altinteger_Elt_method(readidx_lgl::class_t, lgl_Elt);
//}
