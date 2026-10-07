param(
    [Parameter(Mandatory = $true)][string]$Title,
    [ValidateSet('restore', 'click', 'state')][string]$Action = 'state',
    [double]$X = 0,
    [double]$Y = 0
)

# Exercise the Windows -> WSLg input path, scoped to a uniquely titled test window.
Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public class BackupWindowInput {
    public delegate bool WindowCallback(IntPtr window, IntPtr parameter);
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int left, top, right, bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct Point { public int x, y; }
    [DllImport("user32.dll")] public static extern bool EnumWindows(WindowCallback callback, IntPtr parameter);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr window, StringBuilder text, int count);
    [DllImport("user32.dll")] public static extern bool ShowWindowAsync(IntPtr window, int command);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr window, out Rect rect);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr window, ref Point point);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extra);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr window);
    [DllImport("user32.dll")] public static extern bool IsZoomed(IntPtr window);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
'@
[void][BackupWindowInput]::SetProcessDPIAware()
$script:testHandle = [IntPtr]::Zero
[void][BackupWindowInput]::EnumWindows({
    param($window, $parameter)
    $text = New-Object System.Text.StringBuilder 512
    [void][BackupWindowInput]::GetWindowText($window, $text, $text.Capacity)
    if ($text.ToString() -eq $Title -or $text.ToString().StartsWith($Title + ' (')) {
        $script:testHandle = $window
        return $false
    }
    return $true
}, [IntPtr]::Zero)
if ($script:testHandle -eq [IntPtr]::Zero) { throw 'Test window not found' }
if ($Action -eq 'restore') {
    [void][BackupWindowInput]::ShowWindowAsync($script:testHandle, 9)
    [void][BackupWindowInput]::SetForegroundWindow($script:testHandle)
}
if ($Action -eq 'click') {
    $rect = New-Object BackupWindowInput+Rect
    $point = New-Object BackupWindowInput+Point
    [void][BackupWindowInput]::GetClientRect($script:testHandle, [ref]$rect)
    $point.x = [int](($rect.right - $rect.left) * $X)
    $point.y = [int](($rect.bottom - $rect.top) * $Y)
    [void][BackupWindowInput]::ClientToScreen($script:testHandle, [ref]$point)
    [void][BackupWindowInput]::SetForegroundWindow($script:testHandle)
    [void][BackupWindowInput]::SetCursorPos($point.x, $point.y)
    [BackupWindowInput]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero)
    [BackupWindowInput]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero)
}
@{
    minimized = [BackupWindowInput]::IsIconic($script:testHandle)
    maximized = [BackupWindowInput]::IsZoomed($script:testHandle)
} | ConvertTo-Json -Compress
