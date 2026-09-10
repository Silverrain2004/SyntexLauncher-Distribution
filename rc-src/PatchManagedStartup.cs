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

using var asm = AssemblyDefinition.ReadAssembly(path, new ReaderParameters { InMemory = true });
var module = asm.MainModule;
var type = module.Types.Single(t => t.FullName == targetTypeName);
var original = type.Methods.Single(m => m.Name == "InitializeAsync" && !m.HasParameters && m.ReturnType.FullName == "System.Threading.Tasks.Task");
if (type.Methods.Any(m => m.Name == "InitializeAsyncDeferredCore" || m.Name == "RunInitializeAsyncDeferred"))
    throw new InvalidOperationException("already patched");

// Preserve the compiler-generated InitializeAsync wrapper exactly in a private clone.
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

// Queue the real initialization as a parameterless callback.
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

// Do NOT depend on SynchronizationContext.Current here. During Avalonia startup it can be null.
// Always post onto Avalonia's UI dispatcher and, when the API exposes the priority parameter,
// explicitly use DispatcherPriority.Background. This lets the MainWindow/layout/render work run
// before potentially long initialization work, independent of whether persistent state already exists.
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

var temporary = path + ".patched";
asm.Write(temporary);
File.Copy(temporary, path, true);
File.Delete(temporary);
Console.WriteLine($"PATCH_OK_AVALONIA_BACKGROUND:{Path.GetFileName(path)}:POST_PARAMS={post.Parameters.Count}");
