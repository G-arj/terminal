// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "Terminal.hpp"
#include "../src/inc/unicode.hpp"

using namespace Microsoft::Terminal::Core;
using namespace Microsoft::Console::Render;
using namespace Microsoft::Console::Types;
using namespace Microsoft::Console::VirtualTerminal;

// Print puts the text in the buffer and moves the cursor
void Terminal::PrintString(std::wstring_view stringView)
{
    _WriteBuffer(stringView);
}

TextAttribute Terminal::GetTextAttributes() const
{
    return _buffer->GetCurrentAttributes();
}

void Terminal::SetTextAttributes(const TextAttribute& attrs)
{
    _buffer->SetCurrentAttributes(attrs);
}

Viewport Terminal::GetBufferSize()
{
    return _buffer->GetSize();
}

void Terminal::SetCursorPosition(short x, short y)
{
    const auto viewport = _GetMutableViewport();
    const auto viewOrigin = viewport.Origin();
    const short absoluteX = viewOrigin.X + x;
    const short absoluteY = viewOrigin.Y + y;
    COORD newPos{ absoluteX, absoluteY };
    viewport.Clamp(newPos);
    _buffer->GetCursor().SetPosition(newPos);
}

COORD Terminal::GetCursorPosition()
{
    const auto absoluteCursorPos = _buffer->GetCursor().GetPosition();
    const auto viewport = _GetMutableViewport();
    const auto viewOrigin = viewport.Origin();
    const short relativeX = absoluteCursorPos.X - viewOrigin.X;
    const short relativeY = absoluteCursorPos.Y - viewOrigin.Y;
    COORD newPos{ relativeX, relativeY };

    // TODO assert that the coord is > (0, 0) && <(view.W, view.H)
    return newPos;
}

// Method Description:
// - Moves the cursor down one line, and possibly also to the leftmost column.
// Arguments:
// - withReturn, set to true if a carriage return should be performed as well.
// Return value:
// - <none>
void Terminal::CursorLineFeed(const bool withReturn)
{
    auto cursorPos = _buffer->GetCursor().GetPosition();

    // since we explicitly just moved down a row, clear the wrap status on the
    // row we just came from
    _buffer->GetRowByOffset(cursorPos.Y).SetWrapForced(false);

    cursorPos.Y++;
    if (withReturn)
    {
        cursorPos.X = 0;
    }
    _AdjustCursorPosition(cursorPos);
}

// Method Description:
// - deletes count characters starting from the cursor's current position
// - it moves over the remaining text to 'replace' the deleted text
// - for example, if the buffer looks like this ('|' is the cursor): [abc|def]
// - calling DeleteCharacter(1) will change it to: [abc|ef],
// - i.e. the 'd' gets deleted and the 'ef' gets shifted over 1 space and **retain their previous text attributes**
// Arguments:
// - count, the number of characters to delete
// Return value:
// - <none>
void Terminal::DeleteCharacter(const size_t count)
{
    SHORT dist;
    THROW_IF_FAILED(SizeTToShort(count, &dist));
    const auto cursorPos = _buffer->GetCursor().GetPosition();
    const auto copyToPos = cursorPos;
    const COORD copyFromPos{ cursorPos.X + dist, cursorPos.Y };
    const auto sourceWidth = _mutableViewport.RightExclusive() - copyFromPos.X;
    SHORT width;
    THROW_IF_FAILED(UIntToShort(sourceWidth, &width));

    // Get a rectangle of the source
    auto source = Viewport::FromDimensions(copyFromPos, width, 1);

    // Get a rectangle of the target
    const auto target = Viewport::FromDimensions(copyToPos, source.Dimensions());
    const auto walkDirection = Viewport::DetermineWalkDirection(source, target);

    auto sourcePos = source.GetWalkOrigin(walkDirection);
    auto targetPos = target.GetWalkOrigin(walkDirection);

    // Iterate over the source cell data and copy it over to the target
    do
    {
        const auto data = OutputCell(*(_buffer->GetCellDataAt(sourcePos)));
        _buffer->Write(OutputCellIterator({ &data, 1 }), targetPos);
    } while (source.WalkInBounds(sourcePos, walkDirection) && target.WalkInBounds(targetPos, walkDirection));
}

// Method Description:
// - Inserts count spaces starting from the cursor's current position, moving over the existing text
// - for example, if the buffer looks like this ('|' is the cursor): [abc|def]
// - calling InsertCharacter(1) will change it to: [abc| def],
// - i.e. the 'def' gets shifted over 1 space and **retain their previous text attributes**
// Arguments:
// - count, the number of spaces to insert
// Return value:
// - <none>
void Terminal::InsertCharacter(const size_t count)
{
    // NOTE: the code below is _extremely_ similar to DeleteCharacter
    // We will want to use this same logic and implement a helper function instead
    // that does the 'move a region from here to there' operation
    // TODO: Github issue #2163
    SHORT dist;
    THROW_IF_FAILED(SizeTToShort(count, &dist));
    const auto cursorPos = _buffer->GetCursor().GetPosition();
    const auto copyFromPos = cursorPos;
    const COORD copyToPos{ cursorPos.X + dist, cursorPos.Y };
    const auto sourceWidth = _mutableViewport.RightExclusive() - copyFromPos.X;
    SHORT width;
    THROW_IF_FAILED(UIntToShort(sourceWidth, &width));

    // Get a rectangle of the source
    auto source = Viewport::FromDimensions(copyFromPos, width, 1);
    const auto sourceOrigin = source.Origin();

    // Get a rectangle of the target
    const auto target = Viewport::FromDimensions(copyToPos, source.Dimensions());
    const auto walkDirection = Viewport::DetermineWalkDirection(source, target);

    auto sourcePos = source.GetWalkOrigin(walkDirection);
    auto targetPos = target.GetWalkOrigin(walkDirection);

    // Iterate over the source cell data and copy it over to the target
    do
    {
        const auto data = OutputCell(*(_buffer->GetCellDataAt(sourcePos)));
        _buffer->Write(OutputCellIterator({ &data, 1 }), targetPos);
    } while (source.WalkInBounds(sourcePos, walkDirection) && target.WalkInBounds(targetPos, walkDirection));
    const auto eraseIter = OutputCellIterator(UNICODE_SPACE, _buffer->GetCurrentAttributes(), dist);
    _buffer->Write(eraseIter, cursorPos);
}

void Terminal::EraseCharacters(const size_t numChars)
{
    const auto absoluteCursorPos = _buffer->GetCursor().GetPosition();
    const auto viewport = _GetMutableViewport();
    const short distanceToRight = viewport.RightExclusive() - absoluteCursorPos.X;
    const short fillLimit = std::min(static_cast<short>(numChars), distanceToRight);
    const auto eraseIter = OutputCellIterator(UNICODE_SPACE, _buffer->GetCurrentAttributes(), fillLimit);
    _buffer->Write(eraseIter, absoluteCursorPos);
}

// Method description:
// - erases a line of text, either from
// 1. beginning to the cursor's position
// 2. cursor's position to end
// 3. beginning to end
// - depending on the erase type
// Arguments:
// - the erase type
// Return value:
// - true if succeeded, false otherwise
bool Terminal::EraseInLine(const ::Microsoft::Console::VirtualTerminal::DispatchTypes::EraseType eraseType)
{
    const auto cursorPos = _buffer->GetCursor().GetPosition();
    const auto viewport = _GetMutableViewport();
    COORD startPos = { 0 };
    startPos.Y = cursorPos.Y;
    // nlength determines the number of spaces we need to write
    DWORD nlength = 0;

    // Determine startPos.X and nlength by the eraseType
    switch (eraseType)
    {
    case DispatchTypes::EraseType::FromBeginning:
        nlength = cursorPos.X - viewport.Left() + 1;
        break;
    case DispatchTypes::EraseType::ToEnd:
        startPos.X = cursorPos.X;
        nlength = viewport.RightExclusive() - startPos.X;
        break;
    case DispatchTypes::EraseType::All:
        startPos.X = viewport.Left();
        nlength = viewport.RightExclusive() - startPos.X;
        break;
    default:
        return false;
    }

    const auto eraseIter = OutputCellIterator(UNICODE_SPACE, _buffer->GetCurrentAttributes(), nlength);

    // Explicitly turn off end-of-line wrap-flag-setting when erasing cells.
    _buffer->Write(eraseIter, startPos, false);
    return true;
}

// Method description:
// - erases text in the buffer in two ways depending on erase type
// 1. 'erases' all text visible to the user (i.e. the text in the viewport)
// 2. erases all the text in the scrollback
// Arguments:
// - the erase type
// Return Value:
// - true if succeeded, false otherwise
bool Terminal::EraseInDisplay(const DispatchTypes::EraseType eraseType)
{
    // Store the relative cursor position so we can restore it later after we move the viewport
    const auto cursorPos = _buffer->GetCursor().GetPosition();
#pragma warning(suppress : 26496) // This is written by ConvertToOrigin, cpp core checks is wrong saying it should be const.
    auto relativeCursor = cursorPos;
    _mutableViewport.ConvertToOrigin(&relativeCursor);

    // Initialize the new location of the viewport
    // the top and bottom parameters are determined by the eraseType
    SMALL_RECT newWin;
    newWin.Left = _mutableViewport.Left();
    newWin.Right = _mutableViewport.RightExclusive();

    if (eraseType == DispatchTypes::EraseType::All)
    {
        // In this case, we simply move the viewport down, effectively pushing whatever text was on the screen into the scrollback
        // and thus 'erasing' the text visible to the user
        const auto coordLastChar = _buffer->GetLastNonSpaceCharacter(_mutableViewport);
        if (coordLastChar.X == 0 && coordLastChar.Y == 0)
        {
            // Nothing to clear, just return
            return true;
        }

        short sNewTop = coordLastChar.Y + 1;

        // Increment the circular buffer only if the new location of the viewport would be 'below' the buffer
        const short delta = (sNewTop + _mutableViewport.Height()) - (_buffer->GetSize().Height());
        for (auto i = 0; i < delta; i++)
        {
            _buffer->IncrementCircularBuffer();
            sNewTop--;
        }

        newWin.Top = sNewTop;
        newWin.Bottom = sNewTop + _mutableViewport.Height();
    }
    else if (eraseType == DispatchTypes::EraseType::Scrollback)
    {
        // We only want to erase the scrollback, and leave everything else on the screen as it is
        // so we grab the text in the viewport and rotate it up to the top of the buffer
        COORD scrollFromPos{ 0, 0 };
        _mutableViewport.ConvertFromOrigin(&scrollFromPos);
        _buffer->ScrollRows(scrollFromPos.Y, _mutableViewport.Height(), -scrollFromPos.Y);

        // Since we only did a rotation, the text that was in the scrollback is now _below_ where we are going to move the viewport
        // and we have to make sure we erase that text
        const auto eraseStart = _mutableViewport.Height();
        const auto eraseEnd = _buffer->GetLastNonSpaceCharacter(_mutableViewport).Y;
        for (SHORT i = eraseStart; i <= eraseEnd; i++)
        {
            _buffer->GetRowByOffset(i).Reset(_buffer->GetCurrentAttributes());
        }

        // Reset the scroll offset now because there's nothing for the user to 'scroll' to
        _scrollOffset = 0;

        newWin.Top = 0;
        newWin.Bottom = _mutableViewport.Height();
    }
    else
    {
        return false;
    }

    // Move the viewport, adjust the scroll bar if needed, and restore the old cursor position
    _mutableViewport = Viewport::FromExclusive(newWin);
    Terminal::_NotifyScrollEvent();
    SetCursorPosition(relativeCursor.X, relativeCursor.Y);

    return true;
}

void Terminal::WarningBell()
{
    _pfnWarningBell();
}

void Terminal::SetWindowTitle(std::wstring_view title)
{
    if (!_suppressApplicationTitle)
    {
        _title.emplace(title);
        _pfnTitleChanged(_title.value());
    }
}

// Method Description:
// - Retrieves the value in the colortable at the specified index.
// Arguments:
// - tableIndex: the index of the color table to retrieve.
// Return Value:
// - the COLORREF value for the color at that index in the table.
COLORREF Terminal::GetColorTableEntry(const size_t tableIndex) const
{
    return _renderSettings.GetColorTableEntry(tableIndex);
}

// Method Description:
// - Updates the value in the colortable at index tableIndex to the new color
//   color. color is a COLORREF, format 0x00BBGGRR.
// Arguments:
// - tableIndex: the index of the color table to update.
// - color: the new COLORREF to use as that color table value.
// Return Value:
// - <none>
void Terminal::SetColorTableEntry(const size_t tableIndex, const COLORREF color)
{
    _renderSettings.SetColorTableEntry(tableIndex, color);

    if (tableIndex == _renderSettings.GetColorAliasIndex(ColorAlias::DefaultBackground))
    {
        _pfnBackgroundColorChanged(color);
    }

    // Repaint everything - the colors might have changed
    _buffer->GetRenderTarget().TriggerRedrawAll();
}

// Method Description:
// - Sets the position in the color table for the given color alias.
// Arguments:
// - alias: the color alias to update.
// - tableIndex: the new position of the alias in the color table.
// Return Value:
// - <none>
void Terminal::SetColorAliasIndex(const ColorAlias alias, const size_t tableIndex)
{
    _renderSettings.SetColorAliasIndex(alias, tableIndex);
}

// Method Description:
// - Sets the cursor style to the given style.
// Arguments:
// - cursorStyle: the style to be set for the cursor
// Return Value:
// - <none>
void Terminal::SetCursorStyle(const DispatchTypes::CursorStyle cursorStyle)
{
    CursorType finalCursorType = CursorType::Legacy;
    bool shouldBlink = false;

    switch (cursorStyle)
    {
    case DispatchTypes::CursorStyle::UserDefault:
        finalCursorType = _defaultCursorShape;
        shouldBlink = true;
        break;
    case DispatchTypes::CursorStyle::BlinkingBlock:
        finalCursorType = CursorType::FullBox;
        shouldBlink = true;
        break;
    case DispatchTypes::CursorStyle::SteadyBlock:
        finalCursorType = CursorType::FullBox;
        shouldBlink = false;
        break;
    case DispatchTypes::CursorStyle::BlinkingUnderline:
        finalCursorType = CursorType::Underscore;
        shouldBlink = true;
        break;
    case DispatchTypes::CursorStyle::SteadyUnderline:
        finalCursorType = CursorType::Underscore;
        shouldBlink = false;
        break;
    case DispatchTypes::CursorStyle::BlinkingBar:
        finalCursorType = CursorType::VerticalBar;
        shouldBlink = true;
        break;
    case DispatchTypes::CursorStyle::SteadyBar:
        finalCursorType = CursorType::VerticalBar;
        shouldBlink = false;
        break;

    default:
        // Invalid argument should be ignored.
        return;
    }

    _buffer->GetCursor().SetType(finalCursorType);
    _buffer->GetCursor().SetBlinkingAllowed(shouldBlink);
}

void Terminal::SetInputMode(const TerminalInput::Mode mode, const bool enabled)
{
    _terminalInput->SetInputMode(mode, enabled);
}

void Terminal::SetRenderMode(const RenderSettings::Mode mode, const bool enabled)
{
    _renderSettings.SetRenderMode(mode, enabled);

    // Repaint everything - the colors will have changed
    _buffer->GetRenderTarget().TriggerRedrawAll();
}

void Terminal::EnableXtermBracketedPasteMode(const bool enabled)
{
    _bracketedPasteMode = enabled;
}

bool Terminal::IsXtermBracketedPasteModeEnabled() const
{
    return _bracketedPasteMode;
}

bool Terminal::IsVtInputEnabled() const
{
    // We should never be getting this call in Terminal.
    FAIL_FAST();
}

void Terminal::SetCursorVisibility(const bool visible)
{
    _buffer->GetCursor().SetIsVisible(visible);
}

void Terminal::EnableCursorBlinking(const bool enable)
{
    _buffer->GetCursor().SetBlinkingAllowed(enable);

    // GH#2642 - From what we've gathered from other terminals, when blinking is
    // disabled, the cursor should remain On always, and have the visibility
    // controlled by the IsVisible property. So when you do a printf "\e[?12l"
    // to disable blinking, the cursor stays stuck On. At this point, only the
    // cursor visibility property controls whether the user can see it or not.
    // (Yes, the cursor can be On and NOT Visible)
    _buffer->GetCursor().SetIsOn(true);
}

void Terminal::CopyToClipboard(std::wstring_view content)
{
    _pfnCopyToClipboard(content);
}

// Method Description:
// - Updates the buffer's current text attributes to start a hyperlink
// Arguments:
// - The hyperlink URI
// - The customID provided (if there was one)
// Return Value:
// - <none>
void Terminal::AddHyperlink(std::wstring_view uri, std::wstring_view params)
{
    auto attr = _buffer->GetCurrentAttributes();
    const auto id = _buffer->GetHyperlinkId(uri, params);
    attr.SetHyperlinkId(id);
    _buffer->SetCurrentAttributes(attr);
    _buffer->AddHyperlinkToMap(uri, id);
}

// Method Description:
// - Updates the buffer's current text attributes to end a hyperlink
// Return Value:
// - <none>
void Terminal::EndHyperlink()
{
    auto attr = _buffer->GetCurrentAttributes();
    attr.SetHyperlinkId(0);
    _buffer->SetCurrentAttributes(attr);
}

// Method Description:
// - Updates the taskbar progress indicator
// Arguments:
// - state: indicates the progress state
// - progress: indicates the progress value
// Return Value:
// - <none>
void Terminal::SetTaskbarProgress(const ::Microsoft::Console::VirtualTerminal::DispatchTypes::TaskbarState state, const size_t progress)
{
    _taskbarState = static_cast<size_t>(state);

    switch (state)
    {
    case DispatchTypes::TaskbarState::Clear:
        // Always set progress to 0 in this case
        _taskbarProgress = 0;
        break;
    case DispatchTypes::TaskbarState::Set:
        // Always set progress to the value given in this case
        _taskbarProgress = progress;
        break;
    case DispatchTypes::TaskbarState::Indeterminate:
        // Leave the progress value unchanged in this case
        break;
    case DispatchTypes::TaskbarState::Error:
    case DispatchTypes::TaskbarState::Paused:
        // In these 2 cases, if the given progress value is 0, then
        // leave the progress value unchanged, unless the current progress
        // value is 0, in which case set it to a 'minimum' value (10 in our case);
        // if the given progress value is greater than 0, then set the progress value
        if (progress == 0)
        {
            if (_taskbarProgress == 0)
            {
                _taskbarProgress = TaskbarMinProgress;
            }
        }
        else
        {
            _taskbarProgress = progress;
        }
        break;
    }

    if (_pfnTaskbarProgressChanged)
    {
        _pfnTaskbarProgressChanged();
    }
}

void Terminal::SetWorkingDirectory(std::wstring_view uri)
{
    _workingDirectory = uri;
}

std::wstring_view Terminal::GetWorkingDirectory()
{
    return _workingDirectory;
}

// Method Description:
// - Saves the current text attributes to an internal stack.
// Arguments:
// - options, cOptions: if present, specify which portions of the current text attributes
//   should be saved. Only a small subset of GraphicsOptions are actually supported;
//   others are ignored. If no options are specified, all attributes are stored.
// Return Value:
// - <none>
void Terminal::PushGraphicsRendition(const VTParameters options)
{
    _sgrStack.Push(_buffer->GetCurrentAttributes(), options);
}

// Method Description:
// - Restores text attributes from the internal stack. If only portions of text attributes
//   were saved, combines those with the current attributes.
// Arguments:
// - <none>
// Return Value:
// - <none>
void Terminal::PopGraphicsRendition()
{
    const TextAttribute current = _buffer->GetCurrentAttributes();
    _buffer->SetCurrentAttributes(_sgrStack.Pop(current));
}
