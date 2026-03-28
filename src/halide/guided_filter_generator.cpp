#include "Halide.h"

using namespace Halide;

class GuidedFilter : public Generator<GuidedFilter> {
public:
    Input<Buffer<double, 2>> luma{"luma"};
    Input<Buffer<double, 2>> confidence{"confidence"};
    Input<int32_t> radius{"radius"};
    Input<double> eps{"eps"};
    Input<double> gain{"gain"};
    Output<Buffer<double, 2>> result{"result"};

    void generate() {
        Var x("x"), y("y");

        Expr w = luma.width();
        Expr h = luma.height();

        // Pixel count for box-mean normalization at image borders.
        // Matches the C++ integral-image approach which shrinks the
        // window at the boundary.
        pixel_count(x, y) = cast<double>(
            (min(x + radius + 1, w) - max(x - radius, Expr(0))) *
            (min(y + radius + 1, h) - max(y - radius, Expr(0))));

        // Zero-padded luma: out-of-bounds pixels contribute 0 to sums,
        // so dividing by the true pixel_count gives the correct mean.
        Func padded = BoundaryConditions::constant_exterior(
            luma, Expr(0.0));

        // Padded I*I
        Func padded_sq("padded_sq");
        padded_sq(x, y) = padded(x, y) * padded(x, y);

        // --- First pass: box sums of I and I*I ---
        RDom rx1(-radius, 2 * radius + 1, "rx1");
        h_sum_I(x, y) = Expr(0.0);
        h_sum_I(x, y) += padded(x + rx1, y);

        RDom ry1(-radius, 2 * radius + 1, "ry1");
        box_I(x, y) = Expr(0.0);
        box_I(x, y) += h_sum_I(x, y + ry1);

        RDom rx2(-radius, 2 * radius + 1, "rx2");
        h_sum_II(x, y) = Expr(0.0);
        h_sum_II(x, y) += padded_sq(x + rx2, y);

        RDom ry2(-radius, 2 * radius + 1, "ry2");
        box_II(x, y) = Expr(0.0);
        box_II(x, y) += h_sum_II(x, y + ry2);

        // Guided-filter coefficients: a, b
        Func mean_I("mean_I"), var_I("var_I");
        mean_I(x, y) = box_I(x, y) / pixel_count(x, y);
        var_I(x, y) = box_II(x, y) / pixel_count(x, y)
                     - mean_I(x, y) * mean_I(x, y);

        gf_a(x, y) = var_I(x, y) / (var_I(x, y) + eps);
        gf_b(x, y) = mean_I(x, y) * (Expr(1.0) - gf_a(x, y));

        // --- Second pass: average a, b over the same window ---
        Func padded_a = BoundaryConditions::constant_exterior(
            gf_a, Expr(0.0), {{Expr(0), w}, {Expr(0), h}});
        Func padded_b = BoundaryConditions::constant_exterior(
            gf_b, Expr(0.0), {{Expr(0), w}, {Expr(0), h}});

        RDom rx3(-radius, 2 * radius + 1, "rx3");
        h_sum_a(x, y) = Expr(0.0);
        h_sum_a(x, y) += padded_a(x + rx3, y);

        RDom ry3(-radius, 2 * radius + 1, "ry3");
        box_a(x, y) = Expr(0.0);
        box_a(x, y) += h_sum_a(x, y + ry3);

        RDom rx4(-radius, 2 * radius + 1, "rx4");
        h_sum_b(x, y) = Expr(0.0);
        h_sum_b(x, y) += padded_b(x + rx4, y);

        RDom ry4(-radius, 2 * radius + 1, "ry4");
        box_b(x, y) = Expr(0.0);
        box_b(x, y) += h_sum_b(x, y + ry4);

        // Final smoothed luma and edge-aware detail transfer.
        Func mean_a("mean_a"), mean_b("mean_b");
        mean_a(x, y) = box_a(x, y) / pixel_count(x, y);
        mean_b(x, y) = box_b(x, y) / pixel_count(x, y);

        Func smoothed("smoothed");
        smoothed(x, y) = mean_a(x, y) * luma(x, y) + mean_b(x, y);

        Func detail("detail");
        detail(x, y) = luma(x, y) - smoothed(x, y);

        // Adaptive gain: more boost in shadows, less in highlights.
        Func intensity_factor("intensity_factor");
        intensity_factor(x, y) = pow(
            max(luma(x, y), Expr(1.0)) / Expr(65535.0), Expr(-0.3));

        Func clamped_ifactor("clamped_ifactor");
        clamped_ifactor(x, y) = clamp(
            intensity_factor(x, y), Expr(0.5), Expr(3.0));

        result(x, y) = luma(x, y)
            + gain * clamped_ifactor(x, y) * confidence(x, y) * detail(x, y);

        // --- Schedule ---
        const int vec = natural_vector_size<double>();

        // First pass box sums → materialise gf_a, gf_b.
        h_sum_I.compute_root()
               .parallel(y).vectorize(x, vec);
        h_sum_I.update(0)
               .parallel(y).vectorize(x, vec);
        box_I.compute_root()
             .parallel(y).vectorize(x, vec);
        box_I.update(0)
             .parallel(y).vectorize(x, vec);

        h_sum_II.compute_root()
                .parallel(y).vectorize(x, vec);
        h_sum_II.update(0)
                .parallel(y).vectorize(x, vec);
        box_II.compute_root()
              .parallel(y).vectorize(x, vec);
        box_II.update(0)
              .parallel(y).vectorize(x, vec);

        gf_a.compute_root()
             .parallel(y).vectorize(x, vec);
        gf_b.compute_root()
             .parallel(y).vectorize(x, vec);

        // Second pass box sums.
        h_sum_a.compute_root()
               .parallel(y).vectorize(x, vec);
        h_sum_a.update(0)
               .parallel(y).vectorize(x, vec);
        box_a.compute_root()
             .parallel(y).vectorize(x, vec);
        box_a.update(0)
             .parallel(y).vectorize(x, vec);

        h_sum_b.compute_root()
               .parallel(y).vectorize(x, vec);
        h_sum_b.update(0)
               .parallel(y).vectorize(x, vec);
        box_b.compute_root()
             .parallel(y).vectorize(x, vec);
        box_b.update(0)
             .parallel(y).vectorize(x, vec);

        // Output.
        result.parallel(y).vectorize(x, vec);
    }

private:
    Func pixel_count{"pixel_count"};
    Func h_sum_I{"h_sum_I"}, box_I{"box_I"};
    Func h_sum_II{"h_sum_II"}, box_II{"box_II"};
    Func gf_a{"gf_a"}, gf_b{"gf_b"};
    Func h_sum_a{"h_sum_a"}, box_a{"box_a"};
    Func h_sum_b{"h_sum_b"}, box_b{"box_b"};
};

HALIDE_REGISTER_GENERATOR(GuidedFilter, guided_filter)
