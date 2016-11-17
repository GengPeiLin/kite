#define NPY_NO_DEPRECATED_API 7

#include "Python.h"
#include "numpy/arrayobject.h"
#include <numpy/npy_math.h>

#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <omp.h>

typedef npy_float32 float32_t;
typedef npy_float64 float64_t;
typedef npy_uint32 uint32_t;

static PyObject *CovarianceExtError;

int good_array(PyObject* o, int typenum, npy_intp size_want, int ndim_want, npy_intp* shape_want) {
    int i;

    if (!PyArray_Check(o)) {
        PyErr_SetString(CovarianceExtError, "not a NumPy array" );
        return 0;
    }

    if (PyArray_TYPE((PyArrayObject*)o) != typenum) {
        PyErr_SetString(CovarianceExtError, "array of unexpected type");
        return 0;
    }

    if (!PyArray_ISCARRAY((PyArrayObject*)o)) {
        PyErr_SetString(CovarianceExtError, "array is not contiguous or not well behaved");
        return 0;
    }

    if (size_want != -1 && size_want != PyArray_SIZE((PyArrayObject*)o)) {
        PyErr_SetString(CovarianceExtError, "array is of unexpected size");
        return 0;
    }


    if (ndim_want != -1 && ndim_want != PyArray_NDIM((PyArrayObject*)o)) {
        PyErr_SetString(CovarianceExtError, "array is of unexpected ndim");
        return 0;
    }

    if (ndim_want != -1 && shape_want != NULL) {
        for (i=0; i<ndim_want; i++) {
            if (shape_want[i] != -1 && shape_want[i] != PyArray_DIMS((PyArrayObject*)o)[i]) {
                PyErr_SetString(CovarianceExtError, "array is of unexpected shape");
                return 0;
            }
        }
    }
    return 1;
}

static float64_t sqr(float64_t x) {
    return x*x;
}

static void calc_distances(float64_t *E, float64_t *N, npy_intp *shape_coord, uint32_t *map, uint32_t nleafs, uint32_t nthreads, float64_t *dists) {
    npy_intp l1row_beg, l1row_end, l1col_beg, l1col_end, il1row, il1col;
    npy_intp l2row_beg, l2row_end, l2col_beg, l2col_end, il2row, il2col;
    npy_intp icl1, icl2, coord_rows, coord_cols;
    npy_intp il1, il2, ndist, l_length;
    uint32_t leaf_subsampling[nleafs], l1hit, l2hit;
    float64_t dist;

    coord_rows = (npy_intp) shape_coord[0];
    coord_cols = (npy_intp) shape_coord[1];

    // Defining adaptive subsampling
    for (il1=0; il1<nleafs; il1++) {
        l_length = map[il1*4+1] - map[il1*4+0];
        leaf_subsampling[il1] = floor(sqrt(l_length)/2);
    }
    if (nthreads == 0)
        nthreads = omp_get_num_procs();

    //printf("coord_matrix: %ldx%ld\n", coord_rows, coord_cols);
    //printf("subsampling: %d\n", subsampling);
    #pragma omp parallel \
        shared (E, N, map, dists, coord_rows, coord_cols, nleafs, leaf_subsampling) \
        private (l1row_beg, l1row_end, l1col_beg, l1col_end, il1row, il1col, icl1, \
                 l2row_beg, l2row_end, l2col_beg, l2col_end, il2row, il2col, icl2, il2, \
                 ndist, dist, l1hit, l2hit) \
        num_threads (nthreads)
    {
        #pragma omp for schedule (dynamic)
        for (il1=0; il1<nleafs; il1++) {
            l1row_beg = map[il1*4+0];
            l1row_end = map[il1*4+1];
            l1col_beg = map[il1*4+2];
            l1col_end = map[il1*4+3];
            // printf("l(%lu): %lu-%lu:%lu-%lu (ss %d)\n", il1, l1row_beg, l1row_end, l1col_beg, l1col_end, leaf_subsampling[il1]);

            for (il2=il1; il2<nleafs; il2++) {
                l2row_beg = map[il2*4+0];
                l2row_end = map[il2*4+1];
                l2col_beg = map[il2*4+2];
                l2col_end = map[il2*4+3];

                l1hit = 0;
                l2hit = 0;

                dist = 0.;
                ndist = 0;

                while(! (l1hit && l2hit)) {
                    for (il1row=l1row_beg; il1row<l1row_end; il1row++) {
                        if (il1row > coord_rows) continue;
                        for (il1col=l1col_beg; il1col<l1col_end; il1col+=leaf_subsampling[il1]) {
                            if (il1col > coord_cols) continue;
                            icl1 = il1row*coord_cols + il1col;
                            if (npy_isnan(E[icl1]) || npy_isnan(N[icl1])) continue;
                            l1hit = 1;

                            for (il2row=l2row_beg; il2row<l2row_end; il2row++) {
                                if (il2row > coord_rows) continue;
                                for (il2col=l2col_beg; il2col<l2col_end; il2col+=leaf_subsampling[il2]) {
                                    if (il2col > coord_cols) continue;
                                    icl2 = il2row*coord_cols + il2col;
                                    if (npy_isnan(E[icl2]) || npy_isnan(N[icl2])) continue;
                                    l2hit = 1;

                                    dist += sqrt(sqr(E[icl1]-E[icl2]) + sqr(N[icl1]-N[icl2]));
                                    // dist += (N[icl1]-N[icl2]);
                                    ndist++;
                                }
                            }
                        }
                    }
                    #pragma omp critical
                    {
                        if (! l1hit)
                            leaf_subsampling[il1] = floor(leaf_subsampling[il1]/2);
                        if (! l2hit)
                            leaf_subsampling[il2] = floor(leaf_subsampling[il2]/2);
                    }
                }
                dists[il1*(nleafs)+il2] = dist/ndist;
                dists[il2*(nleafs)+il1] = dist/ndist;
            }
        }
    }
}

static PyObject* w_distances(PyObject *dummy, PyObject *args) {
    PyObject *x_arr, *y_arr, *map_arr;
    PyArrayObject *c_x_arr, *c_y_arr, *c_map_arr, *dists_arr;

    float64_t *x, *y, *dists;
    uint32_t *map, nthreads;
    npy_intp shape_coord[2], shape_dist[2], nleafs;
    npy_intp shape_want_map[2] = {-1, 4};

    if (! PyArg_ParseTuple(args, "OOOI", &x_arr, &y_arr, &map_arr, &nthreads)) {
        PyErr_SetString(CovarianceExtError, "usage: distances(X, Y, map, nthreads)");
        return NULL;
    }

    if (! good_array(x_arr, NPY_FLOAT64, -1, 2, NULL))
        return NULL;
    if (! good_array(y_arr, NPY_FLOAT64, -1, 2, NULL))
        return NULL;
    if (! good_array(map_arr, NPY_UINT32, -1, 2, shape_want_map))
        return NULL;

    c_x_arr = PyArray_GETCONTIGUOUS((PyArrayObject*) x_arr);
    c_y_arr = PyArray_GETCONTIGUOUS((PyArrayObject*) y_arr);
    c_map_arr = PyArray_GETCONTIGUOUS((PyArrayObject*) map_arr);


    if (PyArray_SIZE(c_x_arr) != PyArray_SIZE(c_y_arr)) {
        PyErr_SetString(CovarianceExtError, "X and Y must have the same size!");
        return NULL;
    }

    x = PyArray_DATA(c_x_arr);
    y = PyArray_DATA(c_y_arr);
    map = PyArray_DATA(c_map_arr);
    nleafs = PyArray_SIZE(c_map_arr)/4;

    shape_coord[0] = (npy_intp) PyArray_DIMS(c_x_arr)[0];
    shape_coord[1] = (npy_intp) PyArray_DIMS(c_x_arr)[1];
    shape_dist[0] = nleafs;
    shape_dist[1] = nleafs;

    dists_arr = (PyArrayObject*) PyArray_EMPTY(2, shape_dist, NPY_FLOAT64, 0);
    // printf("size distance matrix: %lu\n", PyArray_SIZE(dists_arr));
    // printf("size coord matrix: %lu\n", PyArray_SIZE(x_arr));
    dists = PyArray_DATA(dists_arr);

    calc_distances(x, y, shape_coord, map, nleafs, nthreads, dists);
    return (PyObject*) dists_arr;
}

static PyMethodDef CovarianceExtMethods[] = {
    {"leaf_distances", w_distances, METH_VARARGS,
     "Calculates mean distances between quadtree leafs." },

    {NULL, NULL, 0, NULL}         /* Sentinel */
};

PyMODINIT_FUNC
initcovariance_ext(void)
{
    PyObject *m;

    m = Py_InitModule("covariance_ext", CovarianceExtMethods);
    if (m == NULL) return;
    import_array();

    CovarianceExtError = PyErr_NewException("covariance_ext.error", NULL, NULL);
    Py_INCREF(CovarianceExtError);  /* required, because other code could remove `error`
                               from the module, what would create a dangling
                               pointer. */
    PyModule_AddObject(m, "CovarianceExtError", CovarianceExtError);
}