#include "clause.hpp"
#include "internal.hpp"
#include "iterator.hpp"
#include "macros.hpp"
#include "message.hpp"
#include "proof.hpp"

#include <algorithm>
#include <cmath>

namespace CaDiCaL {

// Code for conflict analysis, e.g., to generate the first UIP clause.  The
// main function is 'analyze' below.  It further uses 'minimize' to minimize
// the first UIP clause, which is in 'minimize.cpp'.  An important side
// effect of conflict analysis is to update the decision queue by bumping
// variables.  Similarly analyzed clauses are bumped to mark them as active.

/*------------------------------------------------------------------------*/

void Internal::learn_empty_clause () {
  assert (!unsat);
  LOG ("learned empty clause");
  if (proof) proof->trace_empty_clause ();
  unsat = true;
}

void Internal::learn_unit_clause (int lit) {
  LOG ("learned unit clause %d", lit);
  if (proof) proof->trace_unit_clause (lit);
  assert (flags (lit).active ());
  flags (lit).status = Flags::FIXED;
  stats.fixed++;
}

/*------------------------------------------------------------------------*/

// Important variables recently used in conflict analysis are 'bumped',
// which means to move them to the front of the VMTF decision queue.  The
// 'bumped' time stamp is updated accordingly.  It is used to determine
// whether the 'queue.assigned' pointer has to be moved in 'unassign'.

void Internal::bump_variable (int lit) {
  const int idx = vidx (lit);
  Link * l = ltab + idx;
  if (!l->next) return;
  queue.dequeue (ltab, l);
  queue.enqueue (ltab, l);
  btab[idx] = ++stats.bumped;
  if (var (idx).level == level) stats.bumplast++;
  LOG ("moved to front %d and bumped %ld", idx, btab[idx]);
  if (!vals[idx]) update_queue_unassigned (idx);
}

struct bumped_earlier {
  Internal * internal;
  bumped_earlier (Internal * i) : internal (i) { }
  bool operator () (int a, int b) {
    return internal->bumped (a) < internal->bumped (b);
  }
};

struct trail_bumped_smaller {
  Internal * internal;
  trail_bumped_smaller (Internal * i) : internal (i) { }
  bool operator () (int a, int b) {
    long c = internal->var (a).trail, s = internal->bumped (a) + c;
    long d = internal->var (b).trail, t = internal->bumped (b) + d;
    if (s < t) return true;
    if (s > t) return false;
    return c < d;
  }
};

void Internal::bump_variables () {
  START (bump);

  if (opts.trailbump &&
      relative (stats.propagations, stats.decisions) > opts.trailbumprops &&
      percent (stats.bumplast, stats.bumped) > opts.trailbumplast) {

    // There are some instances (for instance the 'newton...' instances),
    // which have a very high number of propagations per decision if we try
    // to maintain previous bump order as much as possible.  They go through
    // easily if more recent propagated variables are bumped last, which
    // also reduces propagations per decision by two orders of magnitude.
    // It seems that this is related to the high percentage of bumped
    // variables on the highest decision level.  So if this percentage is
    // high and we have many propagations per decision, then we take the
    // assignment order into account too by comparing with respect to the
    // sum of bumped and trail order. For the instances mentioned above just
    // comparing with respect to assignment order (e.g., trail height when
    // assigned) would work too, but this is in general less robust and thus
    // we use the sum instead with the trail height as (stable) tie-breaker.

    sort (analyzed.begin (), analyzed.end (), trail_bumped_smaller (this));
    stats.trailbumped++;

  } else {

    // Otherwise the default is to bump the variable in the order they are
    // in the current decision queue.  This maintains relative order between
    // bumped variables in the queue and seems to work best for those
    // instance with smaller number of bumped variables on the last decision
    // level.

    sort (analyzed.begin (), analyzed.end (), bumped_earlier (this));
  }
  for (const_int_iterator i = analyzed.begin (); i != analyzed.end (); i++)
    bump_variable (*i);

  STOP (bump);
}

/*------------------------------------------------------------------------*/

// Clause activity is replaced by a move-to-front scheme as well with
// 'analyzed' as time stamp.  Only long and high glue clauses are stamped
// since small or low glue clauses are kept anyhow (and do not actually have
// a 'analyzed' field).  We keep the relative order of bumped clauses by
// sorting them first.

inline void Internal::bump_clause (Clause * c) {
  assert (c->have_analyzed);
  c->analyzed () = ++stats.analyzed;
}

inline void Internal::bump_resolved_clauses () {
  START (bump);
  sort (resolved.begin (), resolved.end (), analyzed_earlier ());
  for (const_clause_iterator i = resolved.begin (); i != resolved.end (); i++)
    bump_clause (*i);
  STOP (bump);
  resolved.clear ();
}

inline void Internal::save_as_resolved_clause (Clause * c) {
  if (!c->redundant) return;
  if (c->hbr) c->used = true;
  if (!c->have_analyzed) return;
  resolved.push_back (c);
}

/*------------------------------------------------------------------------*/

// During conflict analysis literals not seen yet either become part of the
// first unique implication point (UIP) clause (if on lower decision level),
// are dropped (if fixed), or are resolved away (if on the current decision
// level and different from the first UIP).  At the same time we update the
// number of seen literals on a decision level.  This helps conflict clause
// minimization.  The number of seen levels is the glucose level (also
// called 'glue', or 'LBD').

inline void Internal::analyze_literal (int lit, int & open) {
  assert (lit);
  Flags & f = flags (lit);
  if (f.seen) return;
  Var & v = var (lit);
  if (!v.level) return;
  assert (val (lit) < 0);
  if (v.level < level) clause.push_back (lit);
  Level & l = control[v.level];
  if (!l.seen++) {
    LOG ("found new level %d contributing to conflict", v.level);
    levels.push_back (v.level);
  }
  if (v.trail < l.trail) l.trail = v.trail;
  f.seen = true;
  analyzed.push_back (lit);
  LOG ("analyzed literal %d assigned at level %d", lit, v.level);
  if (v.level == level) open++;
}

inline void
Internal::analyze_reason (int lit, Clause * reason, int & open) {
  assert (reason);
  save_as_resolved_clause (reason);
  const const_literal_iterator end = reason->end ();
  const_literal_iterator j = reason->begin ();
  int other;
  while (j != end)
    if ((other = *j++) != lit)
      analyze_literal (other, open);
}

/*------------------------------------------------------------------------*/

void Internal::clear_seen () {
  for (const_int_iterator i = analyzed.begin (); i != analyzed.end (); i++) {
    Flags & f = flags (*i);
    assert (f.seen);
    f.seen = false;
    assert (!f.keep);
    assert (!f.poison);
    assert (!f.removable);
  }
  analyzed.clear ();
}

void Internal::clear_levels () {
  for (const_int_iterator i = levels.begin (); i != levels.end (); i++)
    control[*i].reset ();
  levels.clear ();
}

/*------------------------------------------------------------------------*/

void Internal::analyze () {
  assert (conflict);
  if (!level) { learn_empty_clause (); return; }

  START (analyze);

  // First derive the first UIP clause.
  //
  Clause * reason = conflict;
  LOG (reason, "analyzing conflict");
  int open = 0, uip = 0, other = 0;
  const_int_iterator i = trail.end ();
  for (;;) {
    if (reason) analyze_reason (uip, reason, open);
    else analyze_literal (other, open);
    while (!flags (uip = *--i).seen)
      ;
    if (!--open) break;
    reason = var (uip).reason;
    LOG (reason, "analyzing %d reason", uip);
  }
  LOG ("first UIP %d", uip);
  clause.push_back (-uip);
  check_learned_clause ();

  // Update glue statistics.
  //
  bump_resolved_clauses ();
  const int glue = (int) levels.size ();
  LOG ("1st UIP clause of size %ld and glue %d",
    (long) clause.size (), glue);
  UPDATE_AVERAGE (fast_glue_avg, glue);
  UPDATE_AVERAGE (slow_glue_avg, glue);

  // Update learned = 1st UIP literals counter.
  //
  int size = (int) clause.size ();
  stats.learned += size;

  // Minimize and optionally shrink 1st UIP clause.
  //
  if (size > 1) {
    if (opts.minimize) minimize_clause ();
    size = (int) clause.size ();
  }

  // Update actual size statistics.
  //
  stats.units    += (size == 1);
  stats.binaries += (size == 2);
  UPDATE_AVERAGE (size_avg, size);

  // Update decision heuristics.
  //
  bump_variables ();

  // Determine back jump level, backtrack and assign flipped literal.
  //
  if (size > 1) {
    sort (clause.begin (), clause.end (), trail_larger (this));
    Clause * driving_clause = new_learned_redundant_clause (glue);
    const int jump = var (clause[1]).level;
    UPDATE_AVERAGE (jump_avg, jump);
    backtrack (jump);
    assign_driving (-uip, driving_clause);
  } else {
    iterating = true;
    UPDATE_AVERAGE (jump_avg, 0);
    backtrack (0);
    assign_unit (-uip);
  }

  // Clean up.
  //
  clear_seen ();
  clause.clear ();
  clear_levels ();
  conflict = 0;

  STOP (analyze);
}

// We wait reporting a learned unit until propagation of that unit is
// completed.  Otherwise the 'i' report gives the number of remaining
// variables before propagating the unit (and hides the actual remaining
// variables after propagating it).

void Internal::iterate () { iterating = false; report ('i'); }

};
