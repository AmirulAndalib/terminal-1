// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "type_traits.h"

namespace til
{
    // The at function declares that you've already sufficiently checked that your array access
    // is in range before retrieving an item inside it at an offset.
    // This is to save double/triple/quadruple testing in circumstances where you are already
    // pivoting on the length of a set and now want to pull elements out of it by offset
    // without checking again.
    // gsl::at will do the check again. As will .at(). And using [] will have a warning in audit.
    // This template is explicitly disabled if T is of type gsl::span, as it would interfere with
    // the overload below.
    template<typename T, typename I>
    constexpr auto at(T&& cont, const I i) noexcept -> decltype(auto)
    {
        if constexpr (ContiguousView<T>)
        {
#pragma warning(suppress : 26481) // Suppress bounds.1 check for doing pointer arithmetic
#pragma warning(suppress : 26482) // Suppress bounds.2 check for indexing with constant expressions
#pragma warning(suppress : 26446) // Suppress bounds.4 check for subscript operator.
            return cont.data()[i];
        }
        else
        {
#pragma warning(suppress : 26482) // Suppress bounds.2 check for indexing with constant expressions
#pragma warning(suppress : 26446) // Suppress bounds.4 check for subscript operator.
#pragma warning(suppress : 26445) // Suppress lifetime check for a reference to gsl::span or std::string_view
            return cont[i];
        }
    }
}
