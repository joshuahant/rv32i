// Matrix multiply benchmark for cache configuration experiments.
//
// Computes C = A * B for N×N integer matrices, ITERS times.
// Access pattern: A rows (stride-1, cache-friendly) × B columns
// (stride-N, cache-unfriendly) — the classic pattern that stresses D$.
//
// Working set: 3 × N × N × 4 bytes
//   N=48 → ~27 KiB  (fits in 32 KiB L1-D$, exceeds 8 KiB)
//   N=64 → ~48 KiB  (exceeds 32 KiB L1-D$)
//
// Validation: matrices are initialised with a deterministic pattern;
// after ITERS runs the checksum is compared to the reference value
// computed on the first pass.

static const int N     = 48;
static const int ITERS = 12;

static int A[N][N];
static int B[N][N];
static int C[N][N];

static void init() {
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++) {
            A[i][j] = (i * 7 + j * 3 + 1) & 0xFF;
            B[i][j] = (i * 5 + j * 11 + 2) & 0xFF;
        }
}

static void matmul() {
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++) {
            int s = 0;
            for (int k = 0; k < N; k++)
                s += A[i][k] * B[k][j];
            C[i][j] = s;
        }
}

static int checksum() {
    int s = 0;
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++)
            s += C[i][j];
    return s;
}

int main() {
    init();

    // First pass — compute reference checksum.
    matmul();
    int ref = checksum();

    // Remaining passes — the benchmark body.
    for (int it = 1; it < ITERS; it++)
        matmul();

    // Verify the result is consistent.
    return (checksum() == ref) ? 0 : 1;
}
