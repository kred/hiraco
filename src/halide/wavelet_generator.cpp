#include "Halide.h"

using namespace Halide;

class AtrousWavelet : public Generator<AtrousWavelet> {
public:
    Input<Buffer<double, 2>> input{"input"};
    Input<int32_t> step{"step"};
    Output<Buffer<double, 2>> approx{"approx"};

    void generate() {
        Var x("x"), y("y");

        // Boundary: repeat edge (clamp to bounds) — matches the original
        // C++ loop which clamps negative/overflowing indices.
        Func clamped = BoundaryConditions::repeat_edge(input);

        // B3-spline 1D kernel: [1, 4, 6, 4, 1] / 16
        // Horizontal pass with dilated kernel (dilation = step).
        h_pass(x, y) = Expr(1.0 / 16.0) * clamped(x - 2 * step, y)
                      + Expr(4.0 / 16.0) * clamped(x - step, y)
                      + Expr(6.0 / 16.0) * clamped(x, y)
                      + Expr(4.0 / 16.0) * clamped(x + step, y)
                      + Expr(1.0 / 16.0) * clamped(x + 2 * step, y);

        // Vertical pass with dilated kernel.
        approx(x, y) = Expr(1.0 / 16.0) * h_pass(x, y - 2 * step)
                      + Expr(4.0 / 16.0) * h_pass(x, y - step)
                      + Expr(6.0 / 16.0) * h_pass(x, y)
                      + Expr(4.0 / 16.0) * h_pass(x, y + step)
                      + Expr(1.0 / 16.0) * h_pass(x, y + 2 * step);

        // --- Schedule ---
        const int vec = natural_vector_size<double>();
        h_pass.compute_root()
              .parallel(y)
              .vectorize(x, vec);
        approx.parallel(y)
              .vectorize(x, vec);
    }

private:
    Func h_pass{"h_pass"};
};

HALIDE_REGISTER_GENERATOR(AtrousWavelet, atrous_wavelet)
