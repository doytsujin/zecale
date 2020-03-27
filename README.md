# Zecale (/zi:kale/)

A general purpose zk-SNARK aggregator leveraging recursive composition of SNARKs.

This project can be used to:
1. Scale privacy preserving solutions like [Zeth](https://github.com/clearmatics/zeth) by aggregating proofs off-chain (i.e. generate a proof of computational integrity of their validity) and settling a batch of transactions via a single transaction on-chain. This is also referred to as "zk-zk-rollups".
2. Wrap proofs for a given statement in order to hide the predicate that was evaluated. In other words, by setting the size of the batch to `1`, a "wrapping" proof is generated to prove the correct verification of another proof on witness the verification key and the public inputs. The wrapping proof can the be settled on-chain. This use is very similar to the "Zero-knowledge EXEcution environment" described in [Zexe](https://eprint.iacr.org/2018/962.pdf).

## Building and running the project:

### Environment

In order to follow the README below, you will need:
- [Docker](https://www.docker.com/get-started)
- [Python3](https://www.python.org/downloads/) (at least version `3.7`)

### Development dependencies (for building outside of the Docker container)

Immediate dependencies are provided as submodules and compiled during the Zeth build.
Ensure submodules are synced (`git submodule update --init --recursive`).

The following libraries are also required to build:

- grpc
- gmp
- boost

#### Build the project

```bash
# Clone this repository:
git clone git@github.com:clearmatics/zecale.git
cd zecale

# Configure your environment
. ./setup_env.sh

# Initialize the submodules
git submodule update --init --recursive

# Compile the aggregator
mkdir build
cd build
cmake ..

# Compile all targets
make

# (optional) Run the unit tests
make test

# (optional) Run the all tests (unit tests, syntax checks, etc)
make check

# Start the aggregator_server process
aggregator_server
```
