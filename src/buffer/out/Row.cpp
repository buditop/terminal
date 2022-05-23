// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "precomp.h"
#include "Row.hpp"
#include "textBuffer.hpp"
#include "../types/inc/convert.hpp"

#include <intrin.h>

#pragma warning(push, 1)

// Routine Description:
// - constructor
// Arguments:
// - rowWidth - the width of the row, cell elements
// - fillAttribute - the default text attribute
// Return Value:
// - constructed object
ROW::ROW(wchar_t* buffer, uint16_t* indices, const unsigned short rowWidth, const TextAttribute& fillAttribute) :
    _chars{ buffer },
    _charsCapacity{ rowWidth },
    _indices{ indices },
    _indicesCount{ rowWidth },
    // til::rle needs at least 1 element for resize_trailing_extent() to work correctly.
    _attr{ std::max<uint16_t>(1, rowWidth), fillAttribute },
    _lineRendition{ LineRendition::SingleWidth },
    _wrapForced{ false },
    _doubleBytePadded{ false }
{
    if (buffer)
    {
        // TODO
        wmemset(_chars, UNICODE_SPACE, _indicesCount);
        std::iota(_indices, _indices + _indicesCount + 1, static_cast<uint16_t>(0));
    }
}

// Routine Description:
// - Sets all properties of the ROW to default values
// Arguments:
// - Attr - The default attribute (color) to fill
// Return Value:
// - <none>
bool ROW::Reset(const TextAttribute& Attr)
{
    wmemset(_chars, UNICODE_SPACE, _indicesCount);
    std::iota(_indices, _indices + _indicesCount + 1, static_cast<uint16_t>(0));

    _attr = { gsl::narrow_cast<uint16_t>(_indicesCount), Attr };
    _lineRendition = LineRendition::SingleWidth;
    _wrapForced = false;
    _doubleBytePadded = false;

    return true;
}

// Routine Description:
// - clears char data in column in row
// Arguments:
// - column - 0-indexed column index
// Return Value:
// - <none>
void ROW::ClearColumn(const size_t column)
{
    THROW_HR_IF(E_INVALIDARG, column >= size());
    ClearCell(column);
}

// Routine Description:
// - writes cell data to the row
// Arguments:
// - it - custom console iterator to use for seeking input data. bool() false when it becomes invalid while seeking.
// - index - column in row to start writing at
// - wrap - change the wrap flag if we hit the end of the row while writing and there's still more data in the iterator.
// - limitRight - right inclusive column ID for the last write in this row. (optional, will just write to the end of row if nullopt)
// Return Value:
// - iterator to first cell that was not written to this row.
OutputCellIterator ROW::WriteCells(OutputCellIterator it, const size_t index, const std::optional<bool> wrap, std::optional<size_t> limitRight)
{
    THROW_HR_IF(E_INVALIDARG, index >= size());
    THROW_HR_IF(E_INVALIDARG, limitRight.value_or(0) >= size());

    // If we're given a right-side column limit, use it. Otherwise, the write limit is the final column index available in the char row.
    const auto finalColumnInRow = limitRight.value_or(size() - 1);

    auto currentColor = it->TextAttr();
    uint16_t colorUses = 0;
    auto colorStarts = gsl::narrow_cast<uint16_t>(index);
    auto currentIndex = colorStarts;

    while (it && currentIndex <= finalColumnInRow)
    {
        // Fill the color if the behavior isn't set to keeping the current color.
        if (it->TextAttrBehavior() != TextAttributeBehavior::Current)
        {
            // If the color of this cell is the same as the run we're currently on,
            // just increment the counter.
            if (currentColor == it->TextAttr())
            {
                ++colorUses;
            }
            else
            {
                // Otherwise, commit this color into the run and save off the new one.
                // Now commit the new color runs into the attr row.
                Replace(colorStarts, currentIndex, currentColor);
                currentColor = it->TextAttr();
                colorUses = 1;
                colorStarts = currentIndex;
            }
        }

        // Fill the text if the behavior isn't set to saying there's only a color stored in this iterator.
        if (it->TextAttrBehavior() != TextAttributeBehavior::StoredOnly)
        {
            const auto fillingLastColumn = currentIndex == finalColumnInRow;

            // TODO: MSFT: 19452170 - We need to ensure when writing any trailing byte that the one to the left
            // is a matching leading byte. Likewise, if we're writing a leading byte, we need to make sure we still have space in this loop
            // for the trailing byte coming up before writing it.

            // If we're trying to fill the first cell with a trailing byte, pad it out instead by clearing it.
            // Don't increment iterator. We'll advance the index and try again with this value on the next round through the loop.
            if (currentIndex == 0 && it->DbcsAttr().IsTrailing())
            {
                ClearCell(currentIndex);
            }
            // If we're trying to fill the last cell with a leading byte, pad it out instead by clearing it.
            // Don't increment iterator. We'll exit because we couldn't write a lead at the end of a line.
            else if (fillingLastColumn && it->DbcsAttr().IsLeading())
            {
                ClearCell(currentIndex);
                SetDoubleBytePadded(true);
            }
            // Otherwise, copy the data given and increment the iterator.
            else
            {
                Replace(currentIndex, it->DbcsAttr(), it->Chars());
                ++it;
            }

            // If we're asked to (un)set the wrap status and we just filled the last column with some text...
            // NOTE:
            //  - wrap = std::nullopt    --> don't change the wrap value
            //  - wrap = true            --> we're filling cells as a steam, consider this a wrap
            //  - wrap = false           --> we're filling cells as a block, unwrap
            if (wrap.has_value() && fillingLastColumn)
            {
                // set wrap status on the row to parameter's value.
                SetWrapForced(*wrap);
            }
        }
        else
        {
            ++it;
        }

        // Move to the next cell for the next time through the loop.
        ++currentIndex;
    }

    // Now commit the final color into the attr row
    if (colorUses)
    {
        Replace(colorStarts, currentIndex, currentColor);
    }

    return it;
}

void ROW::Replace(size_t x, size_t width, const std::wstring_view& chars)
{
    const auto new0 = std::min(x, _indicesCount);
    const auto new1 = std::min(new0 + width, _indicesCount);
    auto old0 = new0;
    auto old1 = new1;
    const auto ch0 = _indices[new0];
    uint16_t ch1;

    for (; old0 != 0 && _indices[old0 - 1] == ch0; --old0)
    {
    }

    for (; (ch1 = _indices[old1]) == ch0; ++old1)
    {
    }

    const auto leadingSpaces = new0 - old0;
    const auto trailingSpaces = old1 - new1;
    const auto insertedChars = chars.size() + leadingSpaces + trailingSpaces;
    const auto newRhs = insertedChars + ch0;
    const auto diff = newRhs - ch1;

    if (diff != 0)
    {
        const auto currentLength = _indices[_indicesCount];
        const auto newLength = _indices[_indicesCount] + diff;

        if (newLength <= _charsCapacity)
        {
            std::copy_n(_chars + ch1, currentLength - ch1, _chars + newRhs);
        }
        else
        {
            const auto newCapacity = std::max(newLength, _charsCapacity + (_charsCapacity >> 1));
            const auto chars = static_cast<wchar_t*>(::operator new[](sizeof(wchar_t) * newCapacity));

            std::copy_n(_chars, ch0, chars);
            std::copy_n(_chars + ch1, currentLength - ch1, chars + newRhs);

            if (_charsCapacity != _indicesCount)
            {
                ::operator delete[](_chars);
            }

            _chars = chars;
            _charsCapacity = newCapacity;
        }

        for (auto it = &_indices[new1], end = &_indices[_indicesCount + 1]; it != end; ++it)
        {
            *it += diff;
        }
    }

    {
        auto it = _chars + ch0;
        it = std::fill_n(it, leadingSpaces, L' ');
        it = std::copy_n(chars.data(), chars.size(), it);
        it = std::fill_n(it, trailingSpaces, L' ');
    }

    {
        auto col = gsl::narrow_cast<uint16_t>(old0);
        auto it = _indices + col;
        for (; col < new0; ++col)
        {
            *it++ = col;
        }
        for (const auto c = col; col < new1; ++col)
        {
            *it++ = c;
        }
        for (; col < old1; ++col)
        {
            *it++ = col;
        }
    }
}

#pragma warning(pop)
