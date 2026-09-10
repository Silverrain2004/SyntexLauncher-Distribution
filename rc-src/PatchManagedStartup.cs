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
if (type.Methods.Any(m => m.Name == "InitializeAsyncDeferredCore")) throw new InvalidOperationException("already patched");
var clone = new MethodDefinition("InitializeAsyncDeferredCore", Mono.Cecil.MethodAttributes.Private | Mono.Cecil.MethodAttributes.HideBySig, module.ImportReference(original.ReturnType));
type.Methods.Add(clone);
clone.Body.InitLocals = original.Body.InitLocals;
clone.Body.MaxStackSize = original.Body.MaxStackSize;
var variableMap = new Dictionary<VariableDefinition, VariableDefinition>();
foreach (var variable in original.Body.Variables) { var copy = new VariableDefinition(module.ImportReference(variable.VariableType)); clone.Body.Variables.Add(copy); variableMap[variable] = copy; }
var instructionMap = new Dictionary<Instruction, Instruction>();
foreach (var instruction in original.Body.Instructions) { var copy = Instruction.Create(OpCodes.Nop); copy.OpCode = instruction.OpCode; clone.Body.Instructions.Add(copy); instructionMap[instruction] = copy; }
for (var index = 0; index < original.Body.Instructions.Count; index++)
{
    var source = original.Body.Instructions[index];
    clone.Body.Instructions[index].Operand = source.Operand switch
    {
        Instruction target => instructionMap[target], Instruction[] targets => targets.Select(target => instructionMap[target]).ToArray(),
        VariableDefinition variable => variableMap[variable], ParameterDefinition parameter => clone.Parameters[parameter.Index], _ => source.Operand
    };
}
foreach (var handler in original.Body.ExceptionHandlers)
{
    clone.Body.ExceptionHandlers.Add(new ExceptionHandler(handler.HandlerType) { CatchType = handler.CatchType, TryStart = handler.TryStart is null ? null : instructionMap[handler.TryStart], TryEnd = handler.TryEnd is null ? null : instructionMap[handler.TryEnd], HandlerStart = handler.HandlerStart is null ? null : instructionMap[handler.HandlerStart], HandlerEnd = handler.HandlerEnd is null ? null : instructionMap[handler.HandlerEnd], FilterStart = handler.FilterStart is null ? null : instructionMap[handler.FilterStart] });
}
original.Body = new Mono.Cecil.Cil.MethodBody(original) { InitLocals = false, MaxStackSize = 1 };
var il = original.Body.GetILProcessor();
il.Append(il.Create(OpCodes.Ldarg_0)); il.Append(il.Create(OpCodes.Call, clone)); il.Append(il.Create(OpCodes.Pop));
var completed = typeof(Task).GetProperty(nameof(Task.CompletedTask), BindingFlags.Public | BindingFlags.Static)!.GetMethod!;
il.Append(il.Create(OpCodes.Call, module.ImportReference(completed))); il.Append(il.Create(OpCodes.Ret));
var temporary = path + ".patched";
asm.Write(temporary); File.Copy(temporary, path, true); File.Delete(temporary);
Console.WriteLine($"PATCH_OK:{Path.GetFileName(path)}");
