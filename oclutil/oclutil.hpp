#ifndef OCLUTIL_OCLUTIL_HPP
#define OCLUTIL_OCLUTIL_HPP

/**
 * \file   oclutil.hpp
 * \author Denis Demidov <ddemidov@ksu.ru>
 * \brief  OpenCL convenience utilities.
 */

/**
\mainpage oclutil

oclutil is header-only template library created for ease of C++ based OpenCL
development. Vector arithmetic and multidevice computation is supported. The
source code is available at https://github.com/ddemidov/oclutil.

\section devlist Selection of compute devices

You can select any number of available compute devices, which satisfy provided
filters. Filter is a functor returning bool and acting on a cl::Device
parameter. Several standard filters are provided, such as device type or name
filter, double precision support etc. Filters can be combined with logical
operators. In the example below all devices with names matching "Radeon" and
supporting double precision are selected:
\code
#include <iostream>
#include <oclutil/oclutil.hpp>
using namespace clu;
int main() {
    auto device = device_list(
        Filter::Name("Radeon") && Filter::DoublePrecision()
        );
    std::cout << device.size() << " GPUs found:" << std::endl;
    for(auto &d : device)
        std::cout << "\t" << d.getInfo<CL_DEVICE_NAME>() << std::endl;
}
\endcode

Often you want not just device list, but initialized OpenCL context with
command queue on each available device. This may be achieved with queue_list()
function:
\code
cl::Context context;
std::vector<cl::CommandQueue> queue;
// Select no more than 2 NVIDIA GPUs:
std::tie(context, queue) = queue_list(
    [](const cl::Device &d) {
        return d.getInfo<CL_DEVICE_VENDOR>() == "NVIDIA Corporation";
    } && Filter::Count(2)
    );
\endcode

\section vector Memory allocation and vector arithmetic

Once you got queue list, you can allocate OpenCL buffers on the associated
devices. clu::vector constructor accepts std::vector of cl::CommandQueue.
The contents of the created vector will be equally partitioned between each
queue (presumably, each of the provided queues is linked with separate device). 
Multi-platform computation is supported (that is, you can spread your vectors
across devices by different vendors), but should be used with caution: all
computations will be performed with the speed of the slowest device selected.
\code
const uint n = 1 << 20;
std::vector<double> x(n);
std::generate(x.begin(), x.end(), [](){ return (double)rand() / RAND_MAX; });

cl::Context context;
std::vector<cl::CommandQueue> queue;
std::tie(context, queue) = queue_list(Filter::Type(CL_DEVICE_TYPE_GPU));

clu::vector<double> X(queue, CL_MEM_READ_ONLY,  x);
clu::vector<double> Y(queue, CL_MEM_READ_WRITE, n);
clu::vector<double> Z(queue, CL_MEM_READ_WRITE, n);
\endcode

You can now use simple vector arithmetic with device vector. For every
expression you use, appropriate kernel is compiled (first time it is
encountered in your program) and called automagically.

Vectors are processed in parallel across all devices they were allocated on:
\code
Y = Const(42);
Z = sqrt(Const(2) * X) + cos(Y);
\endcode

You can copy the result back to host or you can use vector::operator[] to
read (or write) vector elements directly. Though latter technique is very
ineffective and should be used for debugging purposes only.
\code
copy(Z, x);
assert(x[42] == Z[42]);
\endcode

Another frequently performed operation is reduction of a vector expression to
single value, such as summation. This can be done with clu::Reductor class:
\code
Reductor<double> sum(queue);

std::cout << sum(Z) << std::endl;
std::cout << sum(sqrt(Const(2) * X) + cos(Y)) << std::endl;
\endcode

\section spmv Sparse matrix-vector multiplication

One of the most common operations in linear algebra is matrix-vector
multiplication. Class clu::SpMat holds representation of a sparse matrix,
spanning several GPUs. In the example below it is used for solution of a system
of linear equations with conjugate gradients method:
\code
typedef double real;
// Solve system of linear equations A u = f with conjugate gradients method.
// Input matrix is represented in CSR format (parameters row, col, and val).
void cg_gpu(
	const std::vector<uint> &row,	// Indices to col and val vectors.
	const std::vector<uint> &col,	// Column numbers of non-zero elements.
	const std::vector<real> &val,	// Values of non-zero elements.
	const std::vector<real> &rhs,	// Right-hand side.
	std::vector<real> &x		// In: initial approximation; out: result.
	)
{
    // Init OpenCL
    cl::Context context;
    std::vector<cl::CommandQueue> queue;

    std::tie(context, queue) = queue_list(Filter::Type(CL_DEVICE_TYPE_GPU));

    // Move data to GPU(s)
    uint n = x.size();
    clu::SpMat<real>  A(queue, n, row.data(), col.data(), val.data());
    clu::vector<real> f(queue, CL_MEM_READ_ONLY,  rhs);
    clu::vector<real> u(queue, CL_MEM_READ_WRITE, x);
    clu::vector<real> r(queue, CL_MEM_READ_WRITE, n);
    clu::vector<real> p(queue, CL_MEM_READ_WRITE, n);
    clu::vector<real> q(queue, CL_MEM_READ_WRITE, n);

    Reductor<real,MAX> max(queue);

    // Solve equation Au = f with conjugate gradients method.
    real rho1, rho2;
    r = f - A * u;

    for(uint iter = 0; max(Abs(r)) > 1e-8 && iter < n; iter++) {
	rho1 = inner_product(r, r);

	if (iter == 0) {
	    p = r;
	} else {
	    real beta = rho1 / rho2;
	    p = r + Const(beta) * p;
	}

	q = A * p;

	real alpha = rho1 / inner_product(p, q);

	u += Const(alpha) * p;
	r -= Const(alpha) * q;

	rho2 = rho1;
    }

    // Get result to host.
    copy(u, x);
}
\endcode

\section custkern Using custom kernels

Custom kernels are of course possible as well. vector::operator(uint)
returns cl::Buffer object for a specified device:
\code
cl::Context context;
std::vector<cl::CommandQueue> queue;
std::tie(context, queue) = queue_list(Filter::Type(CL_DEVICE_TYPE_GPU));

const uint n = 1 << 20;
clu::vector<float> x(queue, CL_MEM_WRITE_ONLY, n);

auto program = build_sources(context, std::string(
    "kernel void dummy(uint size, global float *x)\n"
    "{\n"
    "    uint i = get_global_id(0);\n"
    "    if (i < size) x[i] = 4.2;\n"
    "}\n"
    ));

for(uint d = 0; d < queue.size(); d++) {
    auto dummy = cl::Kernel(program, "dummy").bind(queue[d], alignup(n, 256), 256);
    dummy((uint)x.part_size(d), x(d));
}

std::cout << sum(x) << std::endl;
\endcode
*/

#ifdef WIN32
#  pragma warning(disable : 4290)
#  define NOMINMAX
#endif

#define __CL_ENABLE_EXCEPTIONS

#include <CL/cl.hpp>
#include <iostream>

#include <oclutil/util.hpp>
#include <oclutil/devlist.hpp>
#include <oclutil/vector.hpp>
#include <oclutil/spmat.hpp>
#include <oclutil/reduce.hpp>

#endif
