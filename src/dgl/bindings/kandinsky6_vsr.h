#ifndef __SD_DGL_BINDINGS_KANDINSKY6_VSR_H__
#define __SD_DGL_BINDINGS_KANDINSKY6_VSR_H__

#include "dgl/value.h"

namespace sd::dgl {
    struct RunState;

    Value kandinsky6_vsr_plan(Values& args, RunState&);
    Value kandinsky6_vsr_blend(Values& args, RunState&);
}  // namespace sd::dgl

#endif  // __SD_DGL_BINDINGS_KANDINSKY6_VSR_H__
