using Mono.Cecil;
using Mono.Cecil.Cil;
using System.Reflection;
using System.Threading.Tasks;

if (args.Length != 1)
    throw new ArgumentException("assembly path or directory required");

const string targetTypeName = "Syntex.Launcher.ViewModels.MainWindowViewModel";
string path;
if (Directory.Exists(args[0]))
{
    path = Directory.EnumerateFiles(args[0], "*.dll", SearchOption.TopDirectoryOnly)
        .FirstOrDefault(candidate =>
        {
            try
            {
                using var probe = AssemblyDefinition.ReadAssembly(candidate, new ReaderParameters { InMemory = true });
                return probe.MainModule.Types.Any(t => t.FullName == targetTypeName);
            }
            catch { return false; }
        }) ?? throw new FileNotFoundException($"Managed Syntex launcher assembly containing {targetTypeName} was not found in {args[0]}.");
}
else path = args[0];

static IEnumerable<TypeDefinition> AllTypes(TypeDefinition root)
{
    yield return root;
    foreach (var nested in root.NestedTypes)
        foreach (var type in AllTypes(nested))
            yield return type;
}

using var asm = AssemblyDefinition.ReadAssembly(path, new ReaderParameters { InMemory = true });
var module = asm.MainModule;
var type = module.Types.Single(t => t.FullName == targetTypeName);
var original = type.Methods.Single(m => m.Name == "InitializeAsync" && !m.HasParameters && m.ReturnType.FullName == "System.Threading.Tasks.Task");
if (type.Methods.Any(m => m.Name == "InitializeAsyncDeferredCore" || m.Name == "RunInitializeAsyncDeferred"))
    throw new InvalidOperationException("already patched");

var clone = new MethodDefinition(
    "InitializeAsyncDeferredCore",
    Mono.Cecil.MethodAttributes.Private | Mono.Cecil.MethodAttributes.HideBySig,
    module.ImportReference(original.ReturnType));
type.Methods.Add(clone);
clone.Body.InitLocals = original.Body.InitLocals;
clone.Body.MaxStackSize = original.Body.MaxStackSize;

var variableMap = new Dictionary<VariableDefinition, VariableDefinition>();
foreach (var variable in original.Body.Variables)
{
    var copy = new VariableDefinition(module.ImportReference(variable.VariableType));
    clone.Body.Variables.Add(copy);
    variableMap[variable] = copy;
}

var instructionMap = new Dictionary<Instruction, Instruction>();
foreach (var instruction in original.Body.Instructions)
{
    var copy = Instruction.Create(OpCodes.Nop);
    copy.OpCode = instruction.OpCode;
    clone.Body.Instructions.Add(copy);
    instructionMap[instruction] = copy;
}

for (var index = 0; index < original.Body.Instructions.Count; index++)
{
    var source = original.Body.Instructions[index];
    clone.Body.Instructions[index].Operand = source.Operand switch
    {
        Instruction target => instructionMap[target],
        Instruction[] targets => targets.Select(target => instructionMap[target]).ToArray(),
        VariableDefinition variable => variableMap[variable],
        ParameterDefinition parameter => clone.Parameters[parameter.Index],
        _ => source.Operand
    };
}

foreach (var handler in original.Body.ExceptionHandlers)
{
    clone.Body.ExceptionHandlers.Add(new ExceptionHandler(handler.HandlerType)
    {
        CatchType = handler.CatchType,
        TryStart = handler.TryStart is null ? null : instructionMap[handler.TryStart],
        TryEnd = handler.TryEnd is null ? null : instructionMap[handler.TryEnd],
        HandlerStart = handler.HandlerStart is null ? null : instructionMap[handler.HandlerStart],
        HandlerEnd = handler.HandlerEnd is null ? null : instructionMap[handler.HandlerEnd],
        FilterStart = handler.FilterStart is null ? null : instructionMap[handler.FilterStart]
    });
}

var deferred = new MethodDefinition(
    "RunInitializeAsyncDeferred",
    Mono.Cecil.MethodAttributes.Private | Mono.Cecil.MethodAttributes.HideBySig,
    module.TypeSystem.Void);
type.Methods.Add(deferred);
var deferredIl = deferred.Body.GetILProcessor();
deferredIl.Append(deferredIl.Create(OpCodes.Ldarg_0));
deferredIl.Append(deferredIl.Create(OpCodes.Call, clone));
deferredIl.Append(deferredIl.Create(OpCodes.Pop));
deferredIl.Append(deferredIl.Create(OpCodes.Ret));

// The shipped App.OnFrameworkInitializationCompleted is async void and awaits InitializeAsync
// before assigning desktop.MainWindow. Make that call return immediately and do the expensive
// initialization later on the UI dispatcher, so the App state machine can reach MainWindow first.
var directory = Path.GetDirectoryName(path) ?? throw new InvalidOperationException("assembly directory missing");
var avaloniaBasePath = Path.Combine(directory, "Avalonia.Base.dll");
if (!File.Exists(avaloniaBasePath))
    throw new FileNotFoundException("Avalonia.Base.dll not found next to launcher assembly", avaloniaBasePath);

using var avalonia = AssemblyDefinition.ReadAssembly(avaloniaBasePath, new ReaderParameters { InMemory = true });
var dispatcherType = avalonia.MainModule.Types.Single(t => t.FullName == "Avalonia.Threading.Dispatcher");
var getUiThread = dispatcherType.Methods.Single(m => m.Name == "get_UIThread" && m.IsStatic && m.Parameters.Count == 0);
var postCandidates = dispatcherType.Methods
    .Where(m => m.Name == "Post" && !m.IsStatic && m.Parameters.Count >= 1 && m.Parameters[0].ParameterType.FullName == "System.Action")
    .ToList();
var post = postCandidates.FirstOrDefault(m => m.Parameters.Count == 1)
    ?? postCandidates.FirstOrDefault(m => m.Parameters.Count == 2 && m.Parameters[1].ParameterType.FullName == "Avalonia.Threading.DispatcherPriority")
    ?? throw new MissingMethodException("Avalonia.Threading.Dispatcher.Post(Action[, DispatcherPriority]) not found");

FieldDefinition? backgroundPriority = null;
if (post.Parameters.Count == 2)
{
    var priorityType = avalonia.MainModule.Types.Single(t => t.FullName == "Avalonia.Threading.DispatcherPriority");
    backgroundPriority = priorityType.Fields.SingleOrDefault(f => f.Name == "Background" && f.IsStatic)
        ?? throw new MissingFieldException("Avalonia.Threading.DispatcherPriority.Background not found");
}

var actionCtor = typeof(Action).GetConstructor(new[] { typeof(object), typeof(IntPtr) })
    ?? throw new MissingMethodException("System.Action delegate constructor missing");
var completed = typeof(Task).GetProperty(nameof(Task.CompletedTask), BindingFlags.Public | BindingFlags.Static)!.GetMethod!;

original.Body = new Mono.Cecil.Cil.MethodBody(original)
{
    InitLocals = false,
    MaxStackSize = 4
};
var il = original.Body.GetILProcessor();
il.Append(il.Create(OpCodes.Call, module.ImportReference(getUiThread)));
il.Append(il.Create(OpCodes.Ldarg_0));
il.Append(il.Create(OpCodes.Ldftn, deferred));
il.Append(il.Create(OpCodes.Newobj, module.ImportReference(actionCtor)));
if (backgroundPriority is not null)
    il.Append(il.Create(OpCodes.Ldsfld, module.ImportReference(backgroundPriority)));
il.Append(il.Create(OpCodes.Callvirt, module.ImportReference(post)));
il.Append(il.Create(OpCodes.Call, module.ImportReference(completed)));
il.Append(il.Create(OpCodes.Ret));

// A previous await inside App.OnFrameworkInitializationCompleted can make Avalonia's lifetime
// continue before MainWindow is assigned. The trace proved that the correct top-level window then
// exists and remains responsive but Visible=false forever. Explicitly Show() the exact window as
// soon as the App state machine assigns it to desktop.MainWindow.
var avaloniaControlsPath = Path.Combine(directory, "Avalonia.Controls.dll");
if (!File.Exists(avaloniaControlsPath))
    throw new FileNotFoundException("Avalonia.Controls.dll not found next to launcher assembly", avaloniaControlsPath);

using var avaloniaControls = AssemblyDefinition.ReadAssembly(avaloniaControlsPath, new ReaderParameters { InMemory = true });
var windowType = avaloniaControls.MainModule.Types.Single(t => t.FullName == "Avalonia.Controls.Window");
var showMethod = windowType.Methods.Single(m => m.Name == "Show" && !m.IsStatic && m.Parameters.Count == 0);
var importedWindowType = module.ImportReference(windowType);
var importedShow = module.ImportReference(showMethod);

var appType = module.Types.Single(t => t.FullName == "Syntex.Launcher.App");
var mainWindowSetterSites = AllTypes(appType)
    .SelectMany(t => t.Methods)
    .Where(m => m.HasBody)
    .SelectMany(m => m.Body.Instructions.Select(i => (Method: m, Instruction: i)))
    .Where(x => x.Instruction.Operand is MethodReference mr
        && mr.Name == "set_MainWindow"
        && mr.DeclaringType.FullName == "Avalonia.Controls.ApplicationLifetimes.IClassicDesktopStyleApplicationLifetime")
    .ToList();

if (mainWindowSetterSites.Count != 1)
    throw new InvalidOperationException($"Expected exactly one desktop.MainWindow assignment in App lifecycle, found {mainWindowSetterSites.Count}.");

var site = mainWindowSetterSites[0];
var body = site.Method.Body;
body.InitLocals = true;
body.MaxStackSize = Math.Max(body.MaxStackSize, 3);
var windowLocal = new VariableDefinition(importedWindowType);
body.Variables.Add(windowLocal);
var appIl = body.GetILProcessor();

// Immediately before set_MainWindow the evaluation stack is [desktop, window]. Preserve the
// window in a local without disturbing that stack, call the setter, then Show() the same instance.
var duplicateWindow = appIl.Create(OpCodes.Dup);
var saveWindow = appIl.Create(OpCodes.Stloc, windowLocal);
appIl.InsertBefore(site.Instruction, duplicateWindow);
appIl.InsertBefore(site.Instruction, saveWindow);
var loadWindow = appIl.Create(OpCodes.Ldloc, windowLocal);
var showWindow = appIl.Create(OpCodes.Callvirt, importedShow);
appIl.InsertAfter(site.Instruction, loadWindow);
appIl.InsertAfter(loadWindow, showWindow);

var temporary = path + ".patched";
asm.Write(temporary);
File.Copy(temporary, path, true);
File.Delete(temporary);
Console.WriteLine($"PATCH_OK_AVALONIA_LIFECYCLE:{Path.GetFileName(path)}:POST_PARAMS={post.Parameters.Count}:PRIORITY=BACKGROUND:EXPLICIT_SHOW=1");
