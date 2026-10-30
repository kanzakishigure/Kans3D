using System;
using System.Collections.Immutable;
using System.Linq;
using System.Text;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.CodeAnalysis.Text;

namespace Kans.Generators
{
    [Generator]
    public sealed class ScriptGenerator : IIncrementalGenerator
    {
        public void Initialize(IncrementalGeneratorInitializationContext context)
        {
            var components = context.AdditionalTextsProvider.Where(f => f.Path.EndsWith("ComponentTypes.def"));
            context.RegisterSourceOutput(components, (output, file) =>
            {
                var text = new StringBuilder("namespace Kans { internal static class ComponentIds {\n");
                foreach (string line in file.GetText()!.ToString().Split('\n'))
                {
                    if (!line.StartsWith("KANS_COMPONENT(")) continue;
                    string[] parts = line.Substring(15).TrimEnd('\r', ')').Split(',');
                    text.Append("internal const uint ").Append(parts[1].Trim()).Append(" = ")
                        .Append(parts[0].Trim()).Append(";\n");
                }
                output.AddSource("ComponentIds.g.cs", SourceText.From(text.Append("} }").ToString(), Encoding.UTF8));
            });
            var classes = context.SyntaxProvider.CreateSyntaxProvider(
                (node, _) => node is ClassDeclarationSyntax c && c.BaseList != null,
                (syntax, _) => syntax.SemanticModel.GetDeclaredSymbol((ClassDeclarationSyntax)syntax.Node) as INamedTypeSymbol)
                .Where(type => type != null && !type.IsAbstract && InheritsEntity(type)).Collect();
            var isScriptProject = context.CompilationProvider.Select((compilation, _) =>
            {
                var entity = compilation.GetTypeByMetadataName("Kans.Entity");
                return entity != null && !SymbolEqualityComparer.Default.Equals(entity.ContainingAssembly, compilation.Assembly);
            });
            context.RegisterSourceOutput(classes.Combine(isScriptProject), (output, value) =>
            {
                if (value.Right) EmitRegistry(output, value.Left);
            });
        }

        private static bool InheritsEntity(INamedTypeSymbol? type)
        {
            for (var parent = type?.BaseType; parent != null; parent = parent.BaseType)
                if (parent.ToDisplayString() == "Kans.Entity") return true;
            return false;
        }

        private static void EmitRegistry(SourceProductionContext output, ImmutableArray<INamedTypeSymbol?> classes)
        {
            var names = classes.Select(c => c!.ToDisplayString()).Distinct().OrderBy(n => n, StringComparer.Ordinal).ToArray();
            var source = new StringBuilder("#nullable enable\nnamespace Kans.Generated { public sealed class ScriptRegistry : global::Kans.Interop.IScriptRegistry {\n");
            source.Append("public string[] Names { get; } = new string[] { ");
            foreach (string name in names) source.Append('"').Append(name).Append("\",");
            source.Append("};\npublic global::Kans.Entity? Create(string name) => name switch {\n");
            foreach (string name in names)
                source.Append('"').Append(name).Append("\" => new global::").Append(name).Append("(),\n");
            source.Append("_ => null };\n");
            source.Append("public uint PhysicsCallbacks(global::Kans.Entity target) {\n");
            string[] callbacks = { "OnFixedUpdate", "OnCollisionEnter", "OnCollisionStay", "OnCollisionExit", "OnTriggerEnter", "OnTriggerStay", "OnTriggerExit", "OnControllerColliderHit", "OnJointBreak" };
            foreach (var type in classes.Where(c => c != null).GroupBy(c => c!.ToDisplayString()).Select(g => g.First()!))
            {
                uint mask = 0;
                for (int i = 0; i < callbacks.Length; ++i)
                    for (var parent = type; parent != null && parent.ToDisplayString() != "Kans.Entity"; parent = parent.BaseType)
                        if (parent.GetMembers(callbacks[i]).OfType<IMethodSymbol>().Any(m => m.IsOverride)) { mask |= 1u << i; break; }
                source.Append("if (target.GetType() == typeof(global::").Append(type.ToDisplayString()).Append(")) return ").Append(mask).Append("u;\n");
            }
            source.Append("return 0; }\n");
            source.Append("public bool InvokeTimeline(global::Kans.Entity target, string method, string payload) {\n");
            int dispatchIndex = 0;
            foreach (var type in classes.Where(c => c != null).GroupBy(c => c!.ToDisplayString()).Select(g => g.First()!))
            {

                foreach (var method in type.GetMembers().OfType<IMethodSymbol>().Where(m =>
                    m.MethodKind == MethodKind.Ordinary && m.DeclaredAccessibility == Accessibility.Public && !m.IsStatic &&
                    !m.IsGenericMethod && m.ReturnsVoid && m.Parameters.Length == 1 &&
                    m.Parameters[0].Type.SpecialType == SpecialType.System_String && m.Parameters[0].RefKind == RefKind.None))
                {
                    string variable = "e" + dispatchIndex++;
                    source.Append("if (target is global::").Append(type.ToDisplayString()).Append(' ').Append(variable)
                        .Append(" && method == \"").Append(method.Name).Append("\") { ").Append(variable).Append(".@")
                        .Append(method.Name).Append("(payload); return true; }\n");
                }
            }
            source.Append("return false; }\n[global::System.Runtime.CompilerServices.ModuleInitializer] public static void Register() => global::Kans.Interop.ScriptRegistry.Register(new ScriptRegistry());\n} }\n");
            output.AddSource("ScriptRegistry.g.cs", SourceText.From(source.ToString(), Encoding.UTF8));
        }
    }
}
