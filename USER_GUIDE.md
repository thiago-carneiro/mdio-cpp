# MDIO User Guide
The goal of this user guide is to provide an introduction on how you may want to use **MDIO** in your own applications.

## Table of contents
- [Getting started](#getting-started)
- [How to include MDIO](#how-to-include-mdio)
  - [What a full CMake might look like for Hello, World!](#what-a-full-cmake-might-look-like-for-hello-world)
- [Linking](#linking)
- [How to compile](#how-to-compile)
  - [What a full compile might look like for Hello, World!](#what-a-full-compile-might-look-like-for-hello-world)
- [Building and installing MDIO](#building-and-installing-mdio)
  - [Standard install](#standard-install)
  - [Monolithic shared library install](#monolithic-shared-library-install)
  - [Targeting a specific microarchitecture](#targeting-a-specific-microarchitecture)
  - [Consuming the installed package](#consuming-the-installed-package)
  - [Consuming the monolith without CMake](#consuming-the-monolith-without-cmake)
- [Concepts](#concepts)
  - [Result based returns](#result-based-returns)
  - [Open options](#open-options)
  - [Variable, VariableData, and Dataset](#variable-variabledata-and-dataset)
- [Example Schema](#example-schema)
- [Constructors](#constructors)
  - [Round-tripping a Dataset](#round-tripping-a-dataset)
- [Slicing](#slicing)
  - [Iterating the chunk grid](#iterating-the-chunk-grid)
- [Read](#read)
- [Write](#write)
- [Efficient Assignment (Advanced)](#efficient-assignment-advanced)
  - [Transforming between variables](#transforming-between-variables)
- [Mutable Metadata](#mutable-metadata)
  - [Computing and publishing statistics](#computing-and-publishing-statistics)
  - [Merging partial statistics](#merging-partial-statistics)
  - [Variable index semantics](#variable-index-semantics)

## Getting started
This user guide will assume that you are working in either the provided [devcontainer](https://github.com/TGSAI/mdio-cpp/.devcontainer) or have your environment configured according to the [README](https://github.com/TGSAI/mdio-cpp/README.md). Please ensure you have the [required tools](https://github.com/TGSAI/mdio-cpp?tab=readme-ov-file#requied-tools) before proceeding. Following these guidelines should ensure a stable and consistent experience and will allow the community to provide better support without any guesswork regarding your environment.

This user guide uses minimal BASH scripting in examples. Commands may need to be altered depending on your operating system.

## How to include MDIO
**MDIO** currently only supports the [CMake](https://cmake.org/) build system. This makes including **MDIO** straightforward in pre-existing modern CMake projects.

1. Ensure you have the `FetchContent` module included in your CMake project.
    ```Cmake
    include(FetchContent)
    ```
2. Select a version of **MDIO**. Main can be expected to be stable but may update unexpectedly. [Tagged](https://github.com/TGSAI/mdio-cpp/tags) versions will also be available if a specific version is desired. New versions will be tagged as significant improvements, utilities, or features are added to the API, as well as when underlying dependencies are updated.
    ```Cmake
    FetchContent_Declare(
      mdio
      GIT_REPOSITORY https://github.com/TGSAI/mdio-cpp.git
      GIT_TAG main
    )
    ```
3. Make the **MDIO** library available to use in your CMake project.
    ```Cmake
    FetchContent_MakeAvailable(mdio)
    ```
4. Link **MDIO** and its internal dependencies. By default, CMake will not properly link all internal dependencies, so we provide some helpful variables to take the guesswork out of this process. Required is the `mdio_INTERNAL_DEPS` variable. See the below section on linking for more details.
    ```Cmake
    target_link_libraries(my_program PRIVATE 
      mdio
      ${mdio_INTERNAL_DEPS}
    )
    ```

### What a full CMake might look like for Hello, World!
```Cmake
cmake_minimum_required(VERSION 3.24)
project(hello_world_project VERSION 1.0.0 LANGUAGES CXX)

# Set the C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Include FetchContent module
include(FetchContent)

# Fetch the mdio-cpp library from the specified tag
FetchContent_Declare(
  mdio
  GIT_REPOSITORY https://github.com/TGSAI/mdio-cpp.git
  GIT_TAG main
)
FetchContent_MakeAvailable(mdio)

# Create an executable target
add_executable(hello_mdio src/hello_mdio.cc)

# Link the mdio library to the executable
target_link_libraries(hello_mdio PRIVATE 
  mdio
  ${mdio_INTERNAL_DEPS}
)
```

## Linking
As mentioned above, linking is not as straight forward as fetching the library and linking it against your binary. We provide a handful of linkable variables that streamline the process.
- `mdio_INTERNAL_DEPS` is required and should come immediately after `mdio` in the linking process
- `mdio_INTERNAL_GCS_DRIVER_DEPS` is only required if using [Google Cloud Store](https://cloud.google.com/storage). It should come after `mdio_INTERNAL_DEPS`.
- `mdio_INTERNAL_S3_DRIVER_DEPS` is only required if using [Amazon S3](https://aws.amazon.com/s3/). It should come after `mdio_INTERNAL_DEPS`.

It is worth noting that both the GCS drivers and S3 drivers can be linked at the same time and order does not matter. It is also notable that the order of inclusion *should not* strictly matter for CMake projects, but maintaining this order will help in quickly troubleshooting any issues you may run into.

## How to compile
1. In your root directory you should make a new directory called `build`.
    ```BASH
    $ mkdir build
    ```
2. Move into the `build` directory.
    ```BASH
    $ cd build
    ```
3. Run CMake. This may take up to several minutes depending on your system.
    ```BASH
    $ cmake ..
    ```
4. Run make. This may take up to several minutes depending on your system the first time compiling. Replace `my_program` with the name of your executable.
    ```BASH
    $ make -j$(nproc) my_program
    ```
5. Run your binary!
    ```BASH
    ./my_program
    ```

### What a full compile might look like for Hello, World!
```BASH
$ cd ~/mdio-cpp/examples/hello_mdio
$ pwd
# /home/BrianMichell/mdio-cpp/examples/hello_mdio
$ mkdir build
$ cd build
$ pwd
# /home/BrianMichell/mdio-cpp/examples/hello_mdio/build
$ cmake ..
# nproc will return the number of processors on your system. This helps speed up the build.
$ make -j$(nproc) hello_mdio
$ ./hello_mdio
```

## Building and installing MDIO
The sections above cover consuming **MDIO** with `FetchContent`, which re-fetches and rebuilds **MDIO** (and Tensorstore/Abseil) as part of every consuming project. If you want to build **MDIO** once and reuse it across multiple projects or container images, you can install it instead and consume it with `find_package(mdio)`.

There are two installable forms:
- **Standard install** - the header-only `mdio::mdio` interface target, matching what `FetchContent` gives you in-tree.
- **Monolithic shared library install** - a single self-contained `libmdio_monolith.so` that bundles Tensorstore and Abseil, exposed as `mdio::monolith`. This is the recommended way to consume an installed **MDIO** from outside the `mdio-cpp` build tree, because it vendors the third-party headers it needs and requires no `FetchContent` network access at consume time.

### Standard install
```BASH
$ mkdir build-install && cd build-install
$ cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/mdio
$ cmake --build . -j$(nproc) --target install
```
This installs the **MDIO** headers, the `mdio::mdio` interface target, and a CMake package config under `/opt/mdio/lib/cmake/mdio`. Because `mdio::mdio` is header-only, a consumer still needs to independently provide the same Tensorstore driver targets **MDIO** was built against (as described in [Linking](#linking)), so this form is best suited to projects that already vendor Tensorstore themselves.

### Monolithic shared library install
The monolithic build requires CMake 3.27 *or better*. Give the build its own scratch directory, separate from the install prefix, and pass both explicitly:
```BASH
$ cmake -S . -B /path/to/scratch/mdio-build \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/path/to/mdio-install \
    -DMDIO_BUILD_MONOLITHIC_SHARED=ON
$ cmake --build /path/to/scratch/mdio-build --target install -j"$(nproc)"
```
A few things worth knowing before you start:
- The configure step is slow. Tensorstore's `bazel_to_cmake` translates the entire dependency graph before any compilation happens, so expect several minutes of apparently idle output (`Proto aspect: ...`, `Unknown library: ...`) with no compiler running. Those messages are normal and not errors.
- Position-independent code is already forced project-wide, so you do not need to add `-fPIC` yourself.
- The `install` target also builds the **MDIO** test binaries. If you only want the library, build the `mdio_monolith` target and then install as a separate step:
```BASH
$ cmake --build /path/to/scratch/mdio-build --target mdio_monolith -j"$(nproc)"
$ cmake --install /path/to/scratch/mdio-build
```

This installs:
- `<libdir>/libmdio_monolith.so` - a single shared object with Tensorstore, Abseil, and the Zarr/file/S3/GCS drivers whole-archived in. Consumers link only this one library, so a process that loads more than one **MDIO**-linked plugin (e.g. via `dlopen`) still gets exactly one copy of Abseil's global state instead of aborting with an ODR violation.
- `include/mdio` - the **MDIO** headers.
- `include/{tensorstore,absl,riegeli,nlohmann_json,half}-src/...` - the third-party headers **MDIO**'s public headers depend on, vendored so consumers don't need their own copies or network access. The JSON schema validator header is installed alongside them as `include/nlohmann/json-schema.hpp`.
- `lib/cmake/mdio` - the CMake package config, including the `mdio::monolith` target.

`<libdir>` is whatever `GNUInstallDirs` selects for the platform: `lib64` on Red Hat family 64-bit distributions, `lib` elsewhere. The CMake package config is always written to `lib/cmake/mdio`, so on a `lib64` platform the library and the package config end up in sibling directories. `find_package(mdio)` handles that on its own, but if you are wiring the install into a hand-written build you should check which one you got, or pin it with `-DCMAKE_INSTALL_LIBDIR=lib`.

The maximum number of slices (`MAX_NUM_SLICES`, see [Slicing](#slicing)) is baked into the installed package at install time. Pass `-DMAX_NUM_SLICES=64` (or whatever value you need) to the first `cmake` invocation above if the default of 32 isn't enough; consumers pick it up automatically and do not need to redefine it.

### Targeting a specific microarchitecture
The dependency closure contains C, C++, and assembly sources, so a `-march` selection has to be given to both the C and the C++ compiler or the two halves of the build disagree:
```BASH
$ cmake -S . -B /path/to/scratch/mdio-build-skylake \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/path/to/mdio-install-skylake \
    -DCMAKE_C_FLAGS="-march=skylake" \
    -DCMAKE_CXX_FLAGS="-march=skylake" \
    -DMDIO_BUILD_MONOLITHIC_SHARED=ON
$ cmake --build /path/to/scratch/mdio-build-skylake --target install -j"$(nproc)"
```
Useful `-march` values are `x86-64` (portable baseline), `sandybridge`, `skylake`, and `armv8.2-a`. The AArch64 build has been exercised with GCC 11, C++17, and `-march=armv8.2-a -O2 -ffast-math`.

Use a separate scratch directory and a separate install prefix per target, both because the CMake cache pins the flags it was configured with and because the resulting `libmdio_monolith.so` is only valid for the architecture it was built for.

### Consuming the installed package
Point `CMAKE_PREFIX_PATH` at your install directory (skip this if you installed to a standard system prefix), then `find_package(mdio)` and link `mdio::monolith`:
```Cmake
cmake_minimum_required(VERSION 3.27)
project(hello_mdio_installed VERSION 1.0.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)

find_package(mdio REQUIRED)

add_executable(hello_mdio src/hello_mdio.cc)
target_link_libraries(hello_mdio PRIVATE mdio::monolith)
```
```BASH
$ cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/mdio/install
$ cmake --build build -j$(nproc)
```
No manual `-I` include paths, `MAX_NUM_SLICES` defines, or Tensorstore driver targets are needed; `mdio::monolith` carries all of it as usage requirements.

Because the monolith is a shared object, the dynamic loader has to be able to find it at run time. Binaries run out of the build tree get that for free from the build RPATH, but anything you install yourself needs either `INSTALL_RPATH` pointed at the **MDIO** `<libdir>` or that directory on the loader's search path.

### Consuming the monolith without CMake
If your build system cannot consume a CMake package, you have to reproduce by hand what `mdio::monolith` would have carried. Compile with the vendored header trees, in this order:
```
-I<prefix>/include
-I<prefix>/include/tensorstore-src
-I<prefix>/include/absl-src
-I<prefix>/include/riegeli-src
-I<prefix>/include/nlohmann_json-src/include
-I<prefix>/include/half-src/include
```
along with `-std=c++17` and `-DMAX_NUM_SLICES=<value the package was installed with>`.

Link against the one library:
```
-L<prefix>/<libdir> -Wl,-rpath,<prefix>/<libdir> -lmdio_monolith -pthread -ldl -lrt -lm
```
That is the whole link line. The Tensorstore driver archives and the `--whole-archive` block that a non-monolithic **MDIO** install requires are already inside the `.so`.

One caveat that is easy to lose an afternoon to: nlohmann json encodes its version in an inline namespace (`json_abi_v3_12_0` for the 3.12.0 headers vendored here). If your toolchain also injects a different nlohmann json version, whichever `-I` comes first wins, and a mismatch shows up as a wall of `undefined reference` errors that no amount of link reordering will fix. Make sure `<prefix>/include/nlohmann_json-src/include` precedes any other nlohmann include path, rather than merely appearing somewhere in the list.

## Concepts
### Result based returns
**MDIO** aims to follow the Google style of [not throwing exceptions](https://google.github.io/styleguide/cppguide.html#Exceptions). Instead, we use result based returns wherever an error state could exist. A trivial example of this design pattern is a simple function that tries to divide two integers, and handles the case of divide-by-zero.

```C++
int divide(int numerator, int denominator) {
  if (denominator == 0) {
    throw std::invalid_argument("Denominator cannot be zero");
  }
  return numerator / denominator;
}

int main() {
  int result;
  try {
    result = divide(10, 0);  // Oops, dividing by zero!
  } catch (const std::invalid_argument& e) {
    std::cerr << "Caught exception: " << e.what() << std::endl;
    return 1;
  }
  std::cout << "10 / 0 = " << result << std::endl;
  return 0;
}
```
Now let's recreate this example with **MDIO** result based returns instead.
```C++
mdio::Result<int> divide(int numerator, int denominator) {
  if (denominator == 0) {
    return absl::InvalidArgumentError("Denominator cannot be zero") ;
  }
  return numerator / denominator;
}

int main() {
  auto result = divide(10, 0);  // Oops, still dividing by zero!
  if (!result.status().ok()) {
    std::cerr << "Got a not-ok result: " << result.status() << std::endl;
    return 1;
  }
  std::cout << "10 / 0 = " << result.value() << std::endl;
  return 0;
}
```

Result based returns have the benefit of streamlined error handling, but do slightly increase the verbosity of your code. Adapting to this design pattern may feel uncomfortable for some, but we have found it very robust and clean.

If you find a case where **MDIO** throws an exception, please submit a [bug report](https://github.com/TGSAI/mdio-cpp/issues).

### Open options
Open options control how we interact with files, and **MDIO** is no different.
- `mdio::constants::kOpen`: Opens an **MDIO** for reading and writing. This is only valid for existing MDIO files and will return an error status if it does not exist.
- `mdio::constants::kCreate`: Opens a new **MDIO** for writing. This will return an error if the file already exists.
- `mdio::constants::kCreateClean`: Opens a new **MDIO** for writing. This <b><u>will</u></b> overwrite existing metadata and stored arrays and should only be used in testing. Before anything is deleted, every variable spec is validated at the MDIO level, so an invalid spec is rejected and leaves the existing store untouched. For local-file stores on POSIX systems the teardown is fast-pathed per variable: each variable's own subtree is emptied directly (`BestEffortPreDeleteLocalStore`), while the dataset-root metadata files and unrelated sibling entries are preserved and the stock per-variable delete pass remains as the backstop. Cloud and other kvstore backends always use the stock pass. Users are strongly encouraged to avoid including this option in any production environment as data could be lost if improperly used.

### Variable, VariableData, and Dataset
An `mdio::Variable` is the C++ representation of the [Dataset model](https://mdio-python.readthedocs.io/en/v1/data_models/version_1.html#mdio.schemas.v1.variable.Variable) Variable. It holds no array data, but will be used to both read and write. This process will be explained in more depth below.

An `mdio::VariableData` is what will be used to manipulate your array data in-memory.

An `mdio::Dataset` represents a collection of Variables, and their relation with one another.

More information about Variables, their underlying constructs, and relation to XArray can be found [here](https://github.com/TGSAI/mdio-cpp/issues/8).

## Example schema
For all the following examples, this will be the constructor metadata.
```JSON
{
  "metadata": {
    "apiVersion": "1.0.0",
    "name": "Demo MDIO",
    "createdOn": "2024-08-01T15:50:00.000000Z"
  },
  "variables": [
    {
      "name": "X",
      "dataType": "uint32",
      "dimensions": [{"name": "X", "size": 51200}]
    },
    {
      "name": "Y",
      "dataType": "uint32",
      "dimensions": [{"name": "Y", "size": 51200}]
    },
    {
      "name": "Grid",
      "dataType": "float32",
      "dimensions": ["X", "Y"],
      "metadata": {
        "chunkGrid": {
          "name": "regular",
          "configuration": { "chunkShape": [256, 256] }
        }
      }
    }
  ]
}
```

## Constructors
Constructors are based entirely off of the [MDIO v1](https://mdio-python.readthedocs.io/en/v1/data_models/version_1.html#reference) Dataset model. Simply specify a JSON schema and provide it to the `mdio::Dataset::from_json()` method along with the desired path (which can be a relative path, absolute path, or even a GCS or S3 path!), and your open options.
#### Example header file defining the get_schema function
```C++
/**
 * @brief A demo method signature in a header file.
 * @return An MDIO schema in JSON.
 */
nlohmann::json get_schema();
```
#### Open a brand-new Dataset
```C++
int main() {
  std::string path = "demo.mdio";
  nlohmann::json schema = get_schema();
  mdio::Future<mdio::Dataset> dsFuture = mdio::Dataset::from_json(schema, path, mdio::constants::kCreate);
  if (!dsFuture.status().ok()) {
    std::cerr << "Failed to open a new dataset\n" << dsFuture.status() << std::endl;
    return 1;
  }
  
  mdio::Dataset ds = dsFuture.value();
  std::cout << ds << std::endl;
  return 0;
}
```
#### Open an existing Dataset
```C++
int main() {
  std::string path = "demo.mdio";
  mdio::Future<mdio::Dataset> dsFuture = mdio::Dataset::Open(path, mdio::constants::kOpen);
  if (!dsFuture.status().ok()) {
    std::cerr << "Failed to open a new dataset\n" << dsFuture.status() << std::endl;
    return 1;
  }

  mdio::Dataset ds = dsFuture.value();
  std::cout << ds << std::endl;
  return 0;
}
```

### Round-tripping a Dataset
`Dataset::to_json()` serializes a Dataset back into the creation JSON consumed by `from_json()`, which makes structural copies of an existing store straightforward. The output carries the logical schema — variable names, data types, dimensions, chunk grids, compressors, long names, coordinates, and the stored user attributes (`statsV1`/`unitsV1`/`attributes`) — and it is validated against the dataset creation schema before being returned, so a successful result can be fed straight back into `from_json()`. It is a structure-only copy: the new store starts empty and is filled by whatever you write into it afterwards.
```C++
int main() {
  mdio::Future<mdio::Dataset> dsFut = mdio::Dataset::Open("demo.mdio", mdio::constants::kOpen);
  if (!dsFut.status().ok()) {
    std::cerr << "Failed to open dataset: " << dsFut.status() << std::endl;
    return 1;
  }
  mdio::Dataset ds = dsFut.value();

  mdio::Result<nlohmann::json> jsonRes = ds.to_json();
  if (!jsonRes.status().ok()) {
    std::cerr << "Failed to serialize dataset: " << jsonRes.status() << std::endl;
    return 1;
  }

  mdio::Future<mdio::Dataset> copyFut = mdio::Dataset::from_json(
      jsonRes.value(), "copy.mdio", mdio::zarr::ZarrVersion::kV3,
      mdio::constants::kCreateClean);
  if (!copyFut.status().ok()) {
    std::cerr << "Failed to create the copy: " << copyFut.status() << std::endl;
    return 1;
  }
  return 0;
}
```

Datasets with header variables are rejected: the creation schema has no representation for them, so serializing them would produce a JSON that silently drops them when re-created.

The optional `defaults` argument is forwarded to the underlying TensorStore spec serialization: with the default (`mdio::IncludeDefaults{}`) members that equal their default values are omitted from the emitted JSON, and `mdio::IncludeDefaults{true}` keeps them. Both variants pass the creation validation and are consumable by `from_json()`.

## Slicing
Slicing in **MDIO** is the concept of getting a subset of the data. This could be anything from a single point to a full [hypercube](https://en.wikipedia.org/wiki/Hypercube). Slicing is non-destructive, meaning that if you have pre-existing data that gets sliced that original data will remain untouched.

Currently **MDIO**-cpp only permits single step slicing to ensure data integrity.

Slicing in **MDIO**-cpp is handled through an `mdio::SliceDescriptor`, which requires 4 inputs per dimension. Those input are:

- Label: The name of the dimension to slice.
- Start: The first index. This is <u><b>inclusive</b></u>.
- Stop: The last index. This is <u><b>exclusive</b></u>.
- Step: The stride from one index to the next. This <u><b>must</b></u> be 1.

Below is an example of how we can construct a slice descriptor for the *X* dimension.
```cpp
mdio::SliceDescriptor xSlice = {
  /*label=*/ "X",
  /*start=*/ 0,
  /*stop= */ 100,
  /*step= */ 1
};
```

A label must be a dimension as outlined by the **MDIO** [dataset V1 model](https://mdio-python.readthedocs.io/en/v1/data_models/version_1.html#mdio.schemas.v1.variable.Variable.dimensions). If the label does not apply to a Variable it will simply be overlooked during the slice (no-op). This means you can safely slice an entire dataset, even if there are Variables with differing dimensions.

The recommended method to apply slices is at the dataset level. This will ensure consistent dimensions across your entire dataset.
1. Construct your slice descriptors.
2. Slice the dataset with the `isel` method.
3. Operate on the returned dataset.

Slicing is curcial when working on large datasets that would not fit into memory for reasons explained in the Read section below.
```C++
int main() {
  std::string path = "demo.mdio";
  mdio::Future<mdio::Dataset> dsFut = mdio::Dataset::Open(path, mdio::constants::kOpen);
  if (!dsFut.status().ok()) {
    std::cerr << "Failed to open dataset: " << dsFut.status() << std::endl;
    return 1;
  }

  mdio::Dataset ds = dsFut.value();
  mdio::SliceDescriptor xSlice = {"X", 0, 100, 1};
  mdio::SliceDescriptor ySlice = {"Y", 50, 150, 1};

  // We can pass any number of slices to the method, in any order.
  mdio::Result<mdio::Dataset> slicedRes = ds.isel(xSlice, ySlice);
  if (!slicedRes.status().ok()) {
    std::cerr << "Failed to slice dataset: " << slicedRes.status() << std::endl;
    return 1;
  }

  mdio::Dataset slicedDs = slicedRes.value();

  std::cout << ds << std::endl;
  std::cout << "==============================" << std::endl;
  std::cout << slicedDs << std::endl;

  return 0;
}
```

### Iterating the chunk grid
`Variable::chunks()` returns a range over the variable's chunk grid — one `tensorstore::Box` per chunk position. Each box carries absolute indices into the variable's index domain (`origin()` and `shape()`), and edge chunks yield partial boxes, never out-of-bounds indices. The grid is derived from the stored chunk shape, so rechunking the store changes the iteration. Traversal is row-major, but the order is not a contract.
```C++
mdio::Result<void> inspect_chunks(mdio::Dataset& ds) {
  // We can use the .at method since we're not reading data from disk
  MDIO_ASSIGN_OR_RETURN(auto gridVariable, ds.variables.at("Grid"));

  MDIO_ASSIGN_OR_RETURN(auto chunks, gridVariable.chunks());
  for (const auto& chunk : chunks) {
    // chunk.origin() and chunk.shape() are absolute domain indices.
    std::cout << "Chunk origin: " << chunk.origin()[0] << std::endl;
  }
  return absl::OkStatus();
}
```

Two overloads restrict the grid to a subset of dimensions (given by label or zero-based index, variadic or as a vector). Every dimension left out of the subset is covered at its full extent, so each box spans one chunk position along the named dimensions and the complete extent along all the others:
```C++
// Full-height column tiles over the X dimension:
MDIO_ASSIGN_OR_RETURN(auto columnChunks, gridVariable.chunks("X"));
```

The grid is anchored at the domain origin: a freshly opened variable's domain is 0-based, so the boxes coincide with the stored chunk grid, while a sliced variable's domain carries an offset and the grid tessellates over that offset domain.

This is API ergonomics, not an I/O pattern: keep the read path on `variable.Read()` (or `tensorstore::Read`); do not adopt one-box-per-chunk as the default read pattern.

## Read
Reading **MDIO** is performed lazily, which means that we will only read a small amount of metadata from disk when the **MDIO** is opened. In order to read the array data from disk, we will need to actively solicit the read. It's important to note that once we solicit the read, all of the data will be read at once in parallel automatically. If the data is sliced before reading, only the [chunk grids](https://mdio-python.readthedocs.io/en/v1/data_models/version_1.html#mdio.schemas.v1.variable.VariableMetadata.chunk_grid) contained within the slice are read.

```C++
/** 
 * @brief Reads an arbitrary Variable into memory.
 * @return An Ok result if the process worked as expected.
 */
mdio::Result<void> read_and_return_result(mdio::Dataset& ds) {
  // Pick the first key in the keys list.
  std::string variableToRead = ds.variables.get_keys().front();
  // Get the mdio::Variable object
  MDIO_ASSIGN_OR_RETURN(auto variable, ds.variables.at(key));
  // Solicit the read. This happens asynchronously so we could do other things while the data is read.
  auto readFuture = variable.Read();
  return readFuture.status();  // We are forcing the future to block here.
}
```
This example isn't very useful on its own, so lets step it up by giving it a type and view the array.
```C++
/** 
 * @brief Reads an arbitrary Variable into memory.
 * @return An Ok result if the process worked as expected.
 */
mdio::Result<void> read_and_return_result(mdio::Dataset& ds) {
  // We elect to use the `get_iterable_accessor` because the order is sorted.
  std::string variableToRead = ds.variables.get_iterable_accessor().front();  // "Grid"
  // We use `.get<T>()` so we can use the `get_data_accessor()` in a simple and easy way.
  MDIO_ASSIGN_OR_RETURN(auto variable, ds.variables.get<mdio::dtypes::float32_t>(variableToRead));
  auto readFuture = variable.Read();
  if (!readFuture.status().ok()) {
    return readFuture.status();
  }
  auto variableData = readFuture.value();

  // This gives us our array
  auto arrayValues = variableData.get_data_accessor();

  for (mdio::Index x=0; x<10; x++) {
    for (mdio::Index y=0; y<10; y++) {
      std::cout << arrayValues({x, y}) << " ";
      // You may also use bracket notation here if you wish
      // std::cout << arrayValues[x][y] << " ";
    }
    std::cout << std::endl;
  }

  return absl::OkStatus();
}
```

## Write
Writing **MDIO** data happens in parallel automatically, just like reading. We also need to have either read the values, or generated them from an empty Variable.

```C++
mdio::Result<void> write_and_return_result(mdio::Dataset& ds) {
  std::string variableToWrite = ds.variables.get_iterable_accessor().front();  // "Grid"
  MDIO_ASSIGN_OR_RETURN(auto variable, ds.variables.get<mdio::dtypes::float32_t>(variableToWrite));
  auto readFuture = variable.Read();

  if (!readFuture.status().ok()) {
    return readFuture.status();
  }
  auto variableData = readFuture.value();

  auto arrayValues = variableData.get_data_accessor();

  float value = 0.0;
  for (mdio::Index x=0; x<10; x++) {
    for (mdio::Index y=0; y<10; y++) {
      // The access pattern is the same as before, but now we're setting instead of reading
      arrayValues({x, y}) = value++;
    }
  }

  // Writing is async, we could be performing more operations here if we wished.
  auto writeFuture = variable.Write(variableData);
  return writeFuture.status();
}
```
Since we are overwriting all of our data anyway, we can also use the `from_variable` function to get our `VariableData` object. This function is only recommended if you are the one originating all the data and must be used with caution. Not being chunk-aligned in writes using the `from_variable` method may result in undefined behavior and is not guarenteed to remain consistent between versions of mdio-cpp.

If you are ever in doubt, use the above method of reading to get the VariableData object.
```C++
mdio::Result<void> overwrite_and_return_result(mdio::Dataset& ds) {
  std::string variableToWrite = ds.variables.get_iterable_accessor().front();  // "Grid"
  MDIO_ASSIGN_OR_RETURN(auto variable, ds.variables.get<mdio::dtypes::float32_t>(variableToWrite));
  // This allows for shorter overall code and should avoid larger reads
  MDIO_ASSIGN_OR_RETURN(auto variableData, mdio::from_variable<mdio::dtypes::float32_t>(variable));

  auto arrayValues = variableData.get_data_accessor();

  float value = 0.0;
  for (mdio::Index x=0; x<10; x++) {
    for (mdio::Index y=0; y<10; y++) {
      arrayValues({x, y}) = value++;
    }
  }

  auto writeFuture = variable.Write(variableData);
  return writeFuture.status();
}
```

## Efficient Assignment (Advanced)
For small datasets, setting data elements one at a time may be reasonable, but as your dataset grows, so too does the time it takes to copy from one array to another. The typical way to handle this is to use the STL `std::memcpy` function. When dealing with full datasets, this works exactly as expected, copy from one address or container to another. If the dataset is sliced, as would be expected for large datasets, there is an additional challenge that is presented. When slicing outside of the logical origin, there is an offset in memory that must be taken into account. **MDIO** does provide a convienent method to getting that offset, but as with any low-level operation care must be taken.
```C++
mdio::Result<void> copy_overwrite_and_return(mdio::Dataset& ds) {
  std::string variableToWrite = ds.variables.get_iterable_accessor().front();  // "Grid"
  MDIO_ASSIGN_OR_RETURN(auto variable, ds.variables.get<mdio::dtypes::float32_t>(variableToWrite));
  MDIO_ASSIGN_OR_RETURN(auto variableData, mdio::from_variable<mdio::dtypes::float32_t>(variable));

  // We will get the raw pointer first, and then cast it to a properly typed pointer.
  auto voidPtr = variableData.get_data_accessor().data();  // Note the `.data()` call to get the raw pointer.
  auto typedPtr = static_cast<mdio::dtypes::float32_t*>(voidPtr); 

  // We'll generate some inert data to copy
  std::vector<mdio::dtypes::float32_t> fill_val(51200);
  for (auto i = 0; i < 51200; i++) {
    fill_val[i] = float(i*.0002);
  }

  const auto offsetValue = variableData.get_flattened_offset();
  std::size_t increment = 0;
  for (auto i=0; i<51200; i++) {
    std::memcpy(&typedPtr[offsetValue], fill_val.data(), sizeof(mdio::dtypes::float32_t)*51200);
    increment += 51200;
  }

  auto writeFuture = variable.Write(variableData);
  return writeFuture.status();
}
```
That example is good for demonstration purposes, but for a practical application it's too inflexible. Lets fix it by slicing our dataset and copying the data.
```C++
mdio::Result<void> copy_overwrite_and_return(mdio::Dataset& ds) {
  mdio::SliceDescriptor xSlice = {"X", 256, 512, 1};
  mdio::SliceDescriptor ySlice = {"Y", 512, 768, 1};

  MDIO_ASSIGN_OR_RETURN(auto slicedDs, ds.isel(xSlice, ySlice));

  std::string variableToWrite = slicedDs.variables.get_iterable_accessor().front();  // "Grid"
  MDIO_ASSIGN_OR_RETURN(auto variable, slicedDs.variables.get<mdio::dtypes::float32_t>(variableToWrite));
  MDIO_ASSIGN_OR_RETURN(auto variableData, mdio::from_variable<mdio::dtypes::float32_t>(variable));

  // We will get the raw pointer first, and then cast it to a properly typed pointer.
  auto voidPtr = variableData.get_data_accessor().data();  // Note the `.data()` call to get the raw pointer.
  auto typedPtr = static_cast<mdio::dtypes::float32_t*>(voidPtr); 

  auto numSamples = variable.num_samples();

  // We'll generate some inert data to copy
  std::vector<mdio::dtypes::float32_t> fill_val(numSamples);
  for (auto i = 0; i < numSamples; i++) {
    fill_val[i] = float(i*.0002);
  }

  const auto offsetValue = variableData.get_flattened_offset();
  std::size_t increment = 0;
  for (auto i=0; i<1; i++) {
    std::memcpy(&typedPtr[offsetValue], fill_val.data(), sizeof(mdio::dtypes::float32_t)*fill_val.size());
    increment += numSamples;
  }

  auto writeFuture = variable.Write(variableData);
  return writeFuture.status();
}
```

### Transforming between variables
`mdio::TransformVariable(src, dst, fn)` transfers `src` into `dst` one element at a time, dtype-erased: source and destination dtypes may differ, and `fn` defines the mapping between them. The transform receives the source element and the destination element buffer as raw bytes and must report the number of bytes it wrote, which must equal the destination element size — a mismatching report fails the transfer instead of silently under- or overflowing the destination buffer. For a structured dtype opened as its void byte view the unit is the whole record; for a scalar dtype (including a field view of a structured dtype) it is one dtype item.
```C++
/** @brief Copies an 8-bit sample variable into a 32-bit float variable. */
mdio::Result<void> widen_samples(mdio::Variable<>& src, mdio::Variable<>& dst) {
  mdio::ElementTransform widen =
      [](std::string_view srcBytes, std::string_view dstBytes)
      -> absl::StatusOr<std::size_t> {
    if (dstBytes.size() != sizeof(mdio::dtypes::float32_t)) {
      return absl::InvalidArgumentError("Destination element is not float32.");
    }
    const auto sample = static_cast<mdio::dtypes::uint8_t>(srcBytes.front());
    mdio::dtypes::float32_t widened = static_cast<float>(sample);
    std::memcpy(const_cast<char*>(dstBytes.data()), &widened, sizeof(widened));
    return sizeof(widened);
  };
  return mdio::TransformVariable(src, dst, widen).status();
}
```

The whole region is transferred in one read-transform-write pass, so memory use scales with the variable's size. For a bounded-memory transfer, slice both variables and transform block by block (each block is one read-compute-write cycle):
```C++
mdio::Result<void> transform_in_blocks(mdio::Variable<>& src,
                                       mdio::Variable<>& dst,
                                       const mdio::Index blockSize,
                                       const mdio::Index depthSize,
                                       const mdio::ElementTransform& fn) {
  for (mdio::Index start = 0; start < depthSize; start += blockSize) {
    const mdio::Index stop = std::min(start + blockSize, depthSize);
    MDIO_ASSIGN_OR_RETURN(auto srcSlice, src.slice({{"depth", start, stop}}));
    MDIO_ASSIGN_OR_RETURN(auto dstSlice, dst.slice({{"depth", start, stop}}));
    MDIO_RETURN_IF_ERROR(mdio::TransformVariable(srcSlice, dstSlice, fn).status());
  }
  return absl::OkStatus();
}
```

The transfer is atomic with respect to the destination: if the transform returns an error or violates the byte-count contract, nothing is written and `dst` keeps its prior contents. Validation errors (rank, shape, or element count mismatch between the variables) are also raised before anything is written.

The returned future composes with `ExecuteWhenReady` like any TensorStore future; observe the outcome through `status()`, which waits for completion.

## Mutable Metadata
You may have noticed that [summary statistics](https://mdio-python.readthedocs.io/en/v1/data_models/version_1.html#mdio.schemas.v1.stats.StatisticsMetadata) is part of the dataset model, but how can you include them in your Variable before you've even seen the data? This thought exercise assumes that the answer is "You can't!". To address this problem we allow a limited portion of the metadata to be changed at the Variable level.

The mutable portion is the Variable's user attributes: `statsV1`, `unitsV1`, and the free-form `attributes` object. Changes are applied in memory with `UpdateAttributes` and become durable only when the owning `Dataset` is committed with `CommitMetadata`. Free-form keys follow the same pattern: fetch the JSON with `GetAttributes()`, modify it, and pass it back to `UpdateAttributes`. If the Variable carries an int32-typed histogram, use the template `UpdateAttributes<mdio::dtypes::int32_t>` so the existing histogram re-parses with its original type (the default is float32).

### Computing and publishing statistics
Do not hand-write a `statsV1` JSON object — compute it with `mdio::ComputeStats`. It reads the Variable and produces the on-disk statsV1 contract: an int32 count, float32 sum/sumSquares/min/max, and a centered-bin histogram whose bin centers are derived from the data range. NaN values are skipped (they are missing, not values); a Variable with no data yields the canonical empty statsV1. Supported dtypes are float32, float64, and the signed/unsigned integers; anything else (e.g. float16, complex, struct arrays) is rejected.

`ComputeStats` does not persist anything. Publishing goes through `UpdateAttributes` — which replaces the whole user attributes — and `CommitMetadata`:
```C++
mdio::Future<void> PublishGridStats(mdio::Dataset& ds) {
  // We can use the .at method since we're not reading data from disk
  MDIO_ASSIGN_OR_RETURN(auto gridVariable, ds.variables.at("Grid"));

  MDIO_ASSIGN_OR_RETURN(auto stats, mdio::ComputeStats(gridVariable));

  // UpdateAttributes replaces the WHOLE user attributes, so start from
  // GetAttributes() to preserve any existing attributes/units.
  nlohmann::json gridAttrs = gridVariable.GetAttributes();
  gridAttrs["statsV1"] = stats.getBindable();
  MDIO_RETURN_IF_ERROR(gridVariable.UpdateAttributes(gridAttrs));

  // The commit step will commit the changes in memory to disk
  return ds.CommitMetadata();
}
```

Caveat (measured downstream): for some store layouts this full round-trip nests the `attributes` key one level deeper per commit. For such layouts use the narrow form `gridVariable.UpdateAttributes({{"statsV1", stats.getBindable()}})` — it avoids the nesting but still replaces the whole user attributes, so include `unitsV1` and `attributes` alongside when they must survive.

### Merging partial statistics
For a Variable too large to read in one pass, or a computation split across workers, compute one partial per disjoint subset and combine them with `mdio::MergeStats`: counts and sums add up, min/max fold, and histograms merge by summing per-bin counts — in any order. Partials with a count of zero are neutral and contribute nothing; if every partial is neutral, the result is the canonical empty statsV1.

Partials computed over their own sub-ranges would each derive different bin centers, so compute every partial with the same explicit bin centers using the two-argument `ComputeStats` overload. Values are assigned to the nearest bin center; a value exactly between two centers goes to the higher one, and values outside the center range are clamped to the first/last bin:
```C++
mdio::Future<void> PublishGridStatsPartitioned(mdio::Dataset& ds) {
  // We can use the .at method since we're not reading data from disk
  MDIO_ASSIGN_OR_RETURN(auto gridVariable, ds.variables.at("Grid"));

  // Every partial must share one histogram: fix the bin centers up front.
  const std::vector<float> binCenters = {5.f, 15.f, 25.f, 35.f, 45.f};

  // One partial per disjoint slice of the Variable. Chunk-aligned
  // partitions work the same way (see "Iterating the chunk grid").
  MDIO_ASSIGN_OR_RETURN(auto upperHalf, gridVariable.slice({{"Y", 0, 25600}}));
  MDIO_ASSIGN_OR_RETURN(auto lowerHalf,
                        gridVariable.slice({{"Y", 25600, 51200}}));
  std::vector<mdio::SummaryStats> partials;
  for (const auto& part : {upperHalf, lowerHalf}) {
    MDIO_ASSIGN_OR_RETURN(auto stats, mdio::ComputeStats(part, binCenters));
    partials.push_back(std::move(stats));
  }

  MDIO_ASSIGN_OR_RETURN(auto merged, mdio::MergeStats(partials));

  nlohmann::json gridAttrs = gridVariable.GetAttributes();
  gridAttrs["statsV1"] = merged.getBindable();
  MDIO_RETURN_IF_ERROR(gridVariable.UpdateAttributes(gridAttrs));
  return ds.CommitMetadata();
}
```

**Histogram binnings.** A statsV1 histogram is either *centered* — `binCenters` plus per-bin `counts` (`mdio::CenteredBinHistogram`) — or *edge-defined* — `binEdges`, `binWidths`, and `counts` (`mdio::EdgeDefinedHistogram`). `ComputeStats` always produces a centered histogram; edge-defined histograms arrive from persisted statsV1 JSON. `MergeStats` accepts either, but every partial that carries data must either carry no histogram at all (the empty binning of the mdio-python import convention) or carry a histogram with exactly the same binning: identical bin centers for centered histograms, identical bin edges and widths for edge-defined ones. Mixing a histogram-carrying partial with an empty-binning one, or supplying incompatible edge-defined specs (different edges or widths), is rejected with an error — partials cannot be rebinned from counts alone.

### Variable index semantics
Zarr v3 stores written by MDIO keep a variable index in the root metadata (the `_mdio_variable_index` attribute) so `Open` can resolve variables without listing the whole store. The index is authoritative: an open of an indexed store sees exactly the variables it lists. Variables added by other tooling (outside MDIO) are invisible to it, and `CommitMetadata` regenerates the index from the variables the `Dataset` holds — excluding them — so a metadata commit on such a store perpetuates the gap. If you add variables externally, remove the `_mdio_variable_index` attribute from the root `zarr.json` before the next open: without an index, `Open` falls back to listing the store and sees every variable.