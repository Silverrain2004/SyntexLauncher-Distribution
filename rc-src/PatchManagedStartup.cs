using Mono.Cecil;
using Mono.Cecil.Cil;
using System.Reflection;
using System.Threading;
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

// Keep the compiler-generated InitializeAsync state-machine wrapper intact in a private clone.
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

// This callback is posted to the current UI SynchronizationContext. Posting is the key:
// the original App await receives an already-completed task, creates/assigns MainWindow and
// returns to Avalonia before the original initialization body begins executing.
var deferred = new MethodDefinition(
    "RunInitializeAsyncDeferred",
    Mono.Cecil.MethodAttributes.Private | Mono.Cecil.MethodAttributes.HideBySig,
    module.TypeSystem.Void);
deferred.Parameters.Add(new ParameterDefinition("state", Mono.Cecil.ParameterAttributes.None, module.TypeSystem.Object));
type.Methods.Add(deferred);
var deferredIl = deferred.Body.GetILProcessor();
deferredIl.Append(deferredIl.Create(OpCodes.Ldarg_0));
deferredIl.Append(deferredIl.Create(OpCodes.Call, clone));
deferredIl.Append(deferredIl.Create(OpCodes.Pop));
deferredIl.Append(deferredIl.Create(OpCodes.Ret));

var syncContextType = typeof(SynchronizationContext);
var getCurrent = syncContextType.GetProperty(nameof(SynchronizationContext.Current), BindingFlags.Public | BindingFlags.Static)!.GetMethod!;
var post = syncContextType.GetMethod(nameof(SynchronizationContext.Post), new[] { typeof(SendOrPostCallback), typeof(object) })!;
var callbackCtor = typeof(SendOrPostCallback).GetConstructor(new[] { typeof(object), typeof(IntPtr) })!;
var completed = typeof(Task).GetProperty(nameof(Task.CompletedTask), BindingFlags.Public | BindingFlags.Static)!.GetMethod!;

original.Body = new Mono.Cecil.Cil.MethodBody(original)
{
    InitLocals = true,
    MaxStackSize = 4
};
var contextVariable = new VariableDefinition(module.ImportReference(syncContextType));
original.Body.Variables.Add(contextVariable);
var il = original.Body.GetILProcessor();

var fallback = il.Create(OpCodes.Ldarg_0);
var completedCall = il.Create(OpCodes.Call, module.ImportReference(completed));

il.Append(il.Create(OpCodes.Call, module.ImportReference(getCurrent)));
il.Append(il.Create(OpCodes.Stloc, contextVariable));
il.Append(il.Create(OpCodes.Ldloc, contextVariable));
il.Append(il.Create(OpCodes.Brfalse, fallback));
il.Append(il.Create(OpCodes.Ldloc, contextVariable));
il.Append(il.Create(OpCodes.Ldarg_0));
il.Append(il.Create(OpCodes.Ldftn, deferred));
il.Append(il.Create(OpCodes.Newobj, module.ImportReference(callbackCtor)));
il.Append(il.Create(OpCodes.Ldnull));
il.Append(il.Create(OpCodes.Callvirt, module.ImportReference(post)));
il.Append(il.Create(OpCodes.Br, completedCall));

// Avalonia normally supplies a UI SynchronizationContext here. This fallback preserves
// compatibility if a non-standard host invokes the view model without one.
il.Append(fallback);
il.Append(il.Create(OpCodes.Call, clone));
il.Append(il.Create(OpCodes.Pop));
il.Append(completedCall);
il.Append(il.Create(OpCodes.Ret));

var temporary = path + ".patched";
asm.Write(temporary);
File.Copy(temporary, path, true);
File.Delete(temporary);
Console.WriteLine($"PATCH_OK_DEFERRED_UI:{Path.GetFileName(path)}");
