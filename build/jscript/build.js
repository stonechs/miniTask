// Run with cscript //nologo build\jscript\build.js [debug|release] [path-to-x64-gcc.exe]
var fso = new ActiveXObject("Scripting.FileSystemObject");
var shell = new ActiveXObject("WScript.Shell");
var root = fso.GetParentFolderName(fso.GetParentFolderName(
    fso.GetParentFolderName(WScript.ScriptFullName)));
shell.CurrentDirectory = root;

function quote(value) {
    return '"' + value + '"';
}

var mode = WScript.Arguments.length ? WScript.Arguments.Item(0) : "debug";
if (mode != "debug" && mode != "release") WScript.Quit(2);
var gcc = WScript.Arguments.length > 1 ? WScript.Arguments.Item(1) : "gcc";
if (!fso.FolderExists("dist")) {
    fso.CreateFolder("dist");
}

var output = mode == "release" ? "dist\\miniTask.exe" : "dist\\miniTask-debug.exe";
var flags = "-m64 -std=c11 -Os -Wall -Wextra -ffunction-sections -fdata-sections" +
    " -fno-stack-protector -nostdlib -mwindows -Wl,--gc-sections,-e,miniTaskStart" +
    (mode == "release" ? " -s" : " -DMINITASK_DIAGNOSTICS");
var command = quote(gcc) + " " + flags + " -o " + quote(output) +
    " src\\minitask.c -lkernel32 -luser32 -lshell32 -lgdi32 -ladvapi32";
WScript.Echo(command);
var compiler = shell.Exec(command);
var errors = compiler.StdErr.ReadAll();
var stdout = compiler.StdOut.ReadAll();
if (errors) WScript.StdErr.Write(errors);
if (stdout) WScript.Echo(stdout);
if (compiler.ExitCode) WScript.Quit(compiler.ExitCode);
var bytes = fso.GetFile(output).Size;
WScript.Echo(output + ": " + bytes + " bytes");
if (mode == "debug" && bytes >= 100000) WScript.Quit(1);
if (mode == "release" && bytes >= 20000) WScript.Quit(1);
