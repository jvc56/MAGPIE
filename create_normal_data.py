import numpy as np
import random
import argparse

def save_normal_data(filename, delta, num_vars, means_stdevs, num_samples):
    with open(filename, 'w') as f:
        f.write(f"{delta}\n")
        f.write(f"{num_vars}\n")
        for mean, stdev in means_stdevs:
            f.write(f"{mean},{stdev}\n")
        f.write(f"{num_samples}\n")
        
        samples = np.random.normal(0, 1, num_samples)
        f.write("\n".join(map(str, samples)))
        f.write("\n")
        rng_samples = np.random.uniform(0, 1, num_samples)
        f.write("\n".join(map(str, rng_samples)))

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Generate and save normal distribution data.")
    parser.add_argument("-d", "--d", type=float, default=random.uniform(0.001, 0.1), help="Delta value")
    parser.add_argument("-n", "--n", type=int, default=random.randint(2, 20), help="Number of normal random variables")
    parser.add_argument("-s", "--s", type=int, default=10000, help="Number of normal samples")
    parser.add_argument("-m", "--mr", type=int, nargs=2, default=[-10, 10], help="Range for random means (min max)")
    parser.add_argument("-v", "--vr", type=int, nargs=2, default=[1, 5], help="Range for random standard deviations (min max)")
    parser.add_argument("-o", "--o", type=str, default="normal_data.txt", help="Output filename")
    args = parser.parse_args()

    means = random.sample(range(args.mr[0], args.mr[1] + 1), args.n)
    means_stdevs = [(mean, random.randint(args.vr[0], args.vr[1])) for mean in means]

    save_normal_data(args.o, args.d, args.n, means_stdevs, args.s)
