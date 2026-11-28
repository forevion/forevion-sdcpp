#include "dgl/bindings/kandinsky6_vsr.h"

#include "dgl/bindings/arguments.h"
#include "dgl/bindings/values.h"
#include "dgl/ops/kandinsky6_vsr.h"

namespace sd::dgl {
    static Value plan_value(const ops::Kandinsky6VSRPlan& plan) {
        std::vector<Value> tiles;
        for (const auto& tile : plan.tiles)
            tiles.push_back(Value::dict({{"x", Value::literal(tile[0])}, {"y", Value::literal(tile[1])}}));
        return Value::dict({{"width", Value::literal(plan.width)},
                            {"height", Value::literal(plan.height)},
                            {"factor", Value::literal(plan.factor)},
                            {"tile_width", Value::literal(plan.tile_width)},
                            {"tile_height", Value::literal(plan.tile_height)},
                            {"tiles", Value::list(std::move(tiles), Kind::Dict)}});
    }

    static ops::Kandinsky6VSRPlan read_plan(const Value& value) {
        ops::Kandinsky6VSRPlan plan;
        const auto& fields = value.fields;
        auto number        = [&](const char* name) { return fields.at(name).scalar.get<int64_t>(); };
        plan.width         = number("width");
        plan.height        = number("height");
        plan.factor        = int(number("factor"));
        plan.tile_width    = number("tile_width");
        plan.tile_height   = number("tile_height");
        for (const auto& tile : fields.at("tiles").items)
            plan.tiles.push_back({tile.fields.at("x").scalar.get<int64_t>(), tile.fields.at("y").scalar.get<int64_t>()});
        if (plan.factor != 2 && plan.factor != 4)
            throw std::runtime_error("invalid Kandinsky 6 VSR plan");
        return plan;
    }

    Value kandinsky6_vsr_plan(Values& args, RunState&) {
        return plan_value(ops::kandinsky6_vsr_plan(get_int(args, "width"), get_int(args, "height"), get_number(args, "scale"),
                                                   get_number(args, "min_overlap")));
    }

    Value kandinsky6_vsr_blend(Values& args, RunState&) {
        return Value::object(make_tensor(ops::kandinsky6_vsr_blend(float_tensor_list(args.at("tiles")), read_plan(args.at("plan")))));
    }
}  // namespace sd::dgl
