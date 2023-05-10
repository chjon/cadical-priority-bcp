#ifndef _score_hpp_INCLUDED
#define _score_hpp_INCLUDED

namespace CaDiCaL {

struct score_smaller {
  Internal * internal;
  score_smaller (Internal * i) : internal (i) { }
  bool operator () (unsigned a, unsigned b);
};

struct score_smaller_bcp {
  Internal * internal;
  score_smaller_bcp (Internal * i) : internal (i) { }
  bool operator () (unsigned a, unsigned b);
};

typedef heap<score_smaller> ScoreSchedule;
typedef heap<score_smaller_bcp> ScoreScheduleBCP;

}

#endif
