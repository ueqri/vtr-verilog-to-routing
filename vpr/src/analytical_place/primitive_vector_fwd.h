#pragma once
/**
 * @file
 * @author  Alex Singer
 * @date    June 2025
 * @brief   Forward declarations for the primitive vector class.
 */

#include <cstddef>
#include "vtr_strong_id.h"

/// @brief A unique dimension in the PrimtiveVector class.
typedef vtr::StrongId<struct primitive_vector_dim_tag, size_t> PrimitiveVectorDim;

// Forward declaration of the Primitive Vector class.
class PrimitiveVector;
