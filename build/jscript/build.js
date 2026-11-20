// Run with cscript //nologo build\jscript\build.js [path-to-x64-gcc.exe]
var fso = new ActiveXObject("Scripting.FileSystemObject");
var shell = new ActiveXObject("WScript.Shell");
var root = fso.GetParentFolderName(fso.GetParentFolderName(
    fso.GetParentFolderName(WScript.ScriptFullName)));
shell.CurrentDirectory = root;

function quote(value) {
    return '"' + value + '"';
}

var gcc = WScript.Arguments.length ? WScript.Arguments.Item(0) : "gcc";
if (!fso.FolderExists("dist")) {
    fso.CreateFolder("dist");
}

var output = "dist\\miniTask-debug.exe";
var flags = "-m64 -std=c11 -Os -g -Wall -Wextra -ffunction-sections -fdata-sections" +
    " -fno-stack-protector -nostdlib -mwindows -Wl,--gc-sections,-e,miniTaskStart" +
    " -DMINITASK_DIAGNOSTICS";
var command = quote(gcc) + " " + flags + " -o " + quote(output) +
    " src\\minitask.c -lkernel32 -luser32 -lshell32 -lgdi32";
WScript.Echo(command);
var compiler = shell.Exec(command);
var errors = compiler.StdErr.ReadAll();
var stdout = compiler.StdOut.ReadAll();
if (errors) WScript.StdErr.Write(errors);
if (stdout) WScript.Echo(stdout);
if (compiler.ExitCode) WScript.Quit(compiler.ExitCode);
var bytes = fso.GetFile(output).Size;
WScript.Echo(output + ": " + bytes + " bytes");
if (bytes >= 100000) WScript.Quit(1);
