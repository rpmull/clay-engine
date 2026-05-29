using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.Emit;
using Microsoft.CodeAnalysis.Text;
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Threading;

if (args.Length < 2)
{
    Console.WriteLine("Usage: ScriptCompiler <inputDir> <outputDll> [--managed-debugging] [references...]");
    return;
}

Console.WriteLine("Args:");
for (int i = 0; i < args.Length; i++)
    Console.WriteLine($"  [{i}] = \"{args[i]}\"");

string inputDir = args[0];
string outputDll = args[1];
string outputPdb = Path.ChangeExtension(outputDll, ".pdb");
var extraRefs = new List<string>();
bool managedDebugging = false;

foreach (var arg in args.Skip(2))
{
    if (string.Equals(arg, "--managed-debugging", StringComparison.OrdinalIgnoreCase))
    {
        managedDebugging = true;
        continue;
    }

    extraRefs.Add(arg);
}

Console.WriteLine($"[ScriptCompiler] Managed debugging enabled: {managedDebugging}");

var ignoredDirectories = new HashSet<string>(StringComparer.OrdinalIgnoreCase)
{
    "bin",
    "obj",
    ".library",
    ".git",
    ".vs"
};

IEnumerable<string> EnumerateScriptFiles(string rootDir)
{
    if (!Directory.Exists(rootDir))
        yield break;

    var pending = new Stack<string>();
    pending.Push(rootDir);

    while (pending.Count > 0)
    {
        var currentDir = pending.Pop();

        IEnumerable<string> childDirs;
        try
        {
            childDirs = Directory.EnumerateDirectories(currentDir);
        }
        catch (Exception ex)
        {
            Console.WriteLine($"[Warning] Failed to enumerate directories in {currentDir}: {ex.Message}");
            continue;
        }

        foreach (var childDir in childDirs)
        {
            var directoryName = Path.GetFileName(childDir);
            if (ignoredDirectories.Contains(directoryName))
                continue;

            pending.Push(childDir);
        }

        IEnumerable<string> scriptFiles;
        try
        {
            scriptFiles = Directory.EnumerateFiles(currentDir, "*.cs", SearchOption.TopDirectoryOnly);
        }
        catch (Exception ex)
        {
            Console.WriteLine($"[Warning] Failed to enumerate scripts in {currentDir}: {ex.Message}");
            continue;
        }

        foreach (var scriptFile in scriptFiles)
            yield return scriptFile;
    }
}

var files = EnumerateScriptFiles(inputDir)
    .OrderBy(path => path, StringComparer.OrdinalIgnoreCase)
    .ToArray();
if (files.Length == 0)
{
    Console.WriteLine("No .cs files found.");
    return;
}

var syntaxTrees = files.Select(file =>
{
    using var stream = new FileStream(file, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
    using var reader = new StreamReader(stream, detectEncodingFromByteOrderMarks: true);
    string sourceText = reader.ReadToEnd();
    var text = SourceText.From(sourceText, reader.CurrentEncoding);
    return CSharpSyntaxTree.ParseText(text, path: file);
}).ToList();

var trustedAssemblies = (AppContext.GetData("TRUSTED_PLATFORM_ASSEMBLIES") as string)?
    .Split(Path.PathSeparator)
    .Where(p => p.Contains("System.") || p.EndsWith("mscorlib.dll") || p.EndsWith("netstandard.dll"))
    .Select(p => MetadataReference.CreateFromFile(p))
    .ToList() ?? new();

foreach (var path in extraRefs)
{
    if (!File.Exists(path))
    {
        Console.WriteLine($"[Warning] Reference not found: {path}");
        continue;
    }
    trustedAssemblies.Add(MetadataReference.CreateFromFile(path));
}

var compilationOptions = new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary)
    .WithOptimizationLevel(managedDebugging ? OptimizationLevel.Debug : OptimizationLevel.Release);

var compilation = CSharpCompilation.Create(Path.GetFileNameWithoutExtension(outputDll))
    .WithOptions(compilationOptions)
    .AddReferences(trustedAssemblies)
    .AddSyntaxTrees(syntaxTrees);

var outputDirectory = Path.GetDirectoryName(outputDll);
if (!string.IsNullOrWhiteSpace(outputDirectory))
    Directory.CreateDirectory(outputDirectory);

// Serialize compiles targeting the same DLL (engine can spawn ScriptCompiler twice in one tick).
// Also retry when Claymore/VS still have the previous assembly mapped.
string mutexName = @"Local\ClaymoreScriptCompiler_" + Convert.ToHexString(
    MD5.HashData(Encoding.UTF8.GetBytes(Path.GetFullPath(outputDll))));
using Mutex compileMutex = new Mutex(initiallyOwned: false, name: mutexName);
bool mutexHeld = false;
try
{
    if (!compileMutex.WaitOne(TimeSpan.FromMinutes(2)))
    {
        Console.WriteLine("[ScriptCompiler] Timed out waiting for compile lock on: " + outputDll);
        Environment.Exit(2);
    }

    mutexHeld = true;

    EmitResult emitResult = default;
    const int maxIoAttempts = 12;
    for (int attempt = 1; attempt <= maxIoAttempts; attempt++)
    {
        try
        {
            if (managedDebugging)
            {
                using var peStream = new FileStream(outputDll, FileMode.Create, FileAccess.Write,
                    FileShare.Read);
                using var pdbStream = new FileStream(outputPdb, FileMode.Create, FileAccess.Write,
                    FileShare.Read);
                var emitOptions = new EmitOptions(debugInformationFormat: DebugInformationFormat.PortablePdb);
                emitResult = compilation.Emit(peStream, pdbStream, options: emitOptions);
            }
            else
            {
                if (File.Exists(outputPdb))
                {
                    try
                    {
                        File.Delete(outputPdb);
                    }
                    catch (Exception ex)
                    {
                        Console.WriteLine($"[Warning] Failed to delete stale PDB {outputPdb}: {ex.Message}");
                    }
                }

                using var peStream = new FileStream(outputDll, FileMode.Create, FileAccess.Write,
                    FileShare.Read);
                emitResult = compilation.Emit(peStream);
            }

            break;
        }
        catch (IOException ex)
        {
            Console.WriteLine(
                $"[ScriptCompiler] Output locked (try {attempt}/{maxIoAttempts}): {ex.Message}");
            if (attempt == maxIoAttempts)
            {
                Console.WriteLine(
                    "[ScriptCompiler] Close the running editor or stop debugging if GameScripts.dll is loaded, then retry.");
                if (mutexHeld)
                {
                    compileMutex.ReleaseMutex();
                    mutexHeld = false;
                }

                Environment.Exit(3);
            }

            Thread.Sleep(350);
        }
    }

    if (!emitResult.Success)
    {
        try
        {
            if (File.Exists(outputPdb))
                File.Delete(outputPdb);
        }
        catch
        {
        }

        Console.ForegroundColor = ConsoleColor.Red;
        foreach (var diag in emitResult.Diagnostics)
            Console.WriteLine(diag.ToString());
        Console.ResetColor();
        if (mutexHeld)
        {
            compileMutex.ReleaseMutex();
            mutexHeld = false;
        }

        Environment.Exit(1);
    }

    Console.ForegroundColor = ConsoleColor.Green;
    Console.WriteLine($"Compiled {files.Length} scripts to {outputDll}");
    if (managedDebugging)
        Console.WriteLine($"Emitted symbols to {outputPdb}");
    Console.ResetColor();
}
finally
{
    if (mutexHeld)
        compileMutex.ReleaseMutex();
}
