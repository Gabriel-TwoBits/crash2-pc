"""Spike: each direct call of a game file made with the signature of its callee's definition (the game's or the host's
IR), as C does where a prototype is in scope: each argument converted (trunc, sext or zext, inttoptr, ptrtoint), one
too many dropped, one missing 0, the result converted back. A wasm call of another signature traps, and LLVM's own
adapters only bitcast. Functions declared as data, for their addresses, become function declarations.

    python fixcalls.py TARGET.ll,... SOURCE.ll...
"""
import re
import sys
from pathlib import Path

DEFINE = re.compile(r"^define ((?:[a-z_]+ )*?)((?:signext |zeroext )?)(\S+) @([\w.$]+)\(([^)]*)\)", re.M)
DECLARE = re.compile(r"^declare .*?@([\w.$]+)\(.*$", re.M)
EXTERNAL = re.compile(r"^@([\w.$]+) = external (?:[a-z_]+ )*global .*$", re.M)
INDIRECT = re.compile(r"^(\s*)(?:(%[\w.]+) = )?((?:tail |musttail |notail )?call )((?:[a-z_]+ )*?)(\S+) (%[\w.]+)\((.*)\)(.*)$")
SPLIT_WORD = re.compile(r"\.word ([A-Za-z_][\w]*)")
NARROW = {"i1": "i32", "i8": "i32", "i16": "i32"}
CALL = re.compile(r"^(\s*)(?:(%[\w.]+) = )?((?:tail |musttail |notail )?call )((?:[a-z_]+ )*?)(\S+) @([\w.$]+)\((.*)\)(.*)$")
# CALL's group holding the callee's name
CALL_CALLEE = 6
INTEGER_BITS = {"i1": 1, "i8": 8, "i16": 16, "i32": 32, "i64": 64}
ATTRIBUTES = re.compile(r"\b(noundef|signext|zeroext|nonnull|noalias|nocapture|readonly|writeonly|dereferenceable\(\d+\)|align \d+)\b ?")


def split_args(text):
    """Arguments split at top-level commas"""
    parts, depth, current = [], 0, ""
    for char in text:
        if char in "([{<":
            depth += 1
        elif char in ")]}>":
            depth -= 1
        else:
            # Any other character
            pass
        if char == "," and depth == 0:
            parts.append(current.strip())
            current = ""
        else:
            current += char
    if current.strip():
        parts.append(current.strip())
    else:
        # No argument
        pass
    return parts


def extension_of(text):
    if "signext" in text:
        return "signext"
    elif "zeroext" in text:
        return "zeroext"
    else:
        return ""


def parse_param(text):
    """(type, extension) of a definition's parameter"""
    return text.strip().split(" ")[0], extension_of(text)


def parse_arg(text):
    """(type, extension, value) of a call's argument"""
    extension = extension_of(text)
    rest = ATTRIBUTES.sub("", text).strip()
    type_name, value = rest.split(" ", 1)
    return type_name, extension, value.strip()


def convert(value, source, target, extension, temp):
    """The instructions turning value of source type into target type, and the converted value"""
    if source == target:
        return [], value
    elif source in INTEGER_BITS and target in INTEGER_BITS:
        if INTEGER_BITS[source] > INTEGER_BITS[target]:
            op = "trunc"
        elif extension == "zeroext":
            op = "zext"
        else:
            op = "sext"
        return [f"{temp} = {op} {source} {value} to {target}"], temp
    elif source in INTEGER_BITS and target == "ptr":
        return [f"{temp} = inttoptr {source} {value} to ptr"], temp
    elif source == "ptr" and target in INTEGER_BITS:
        return [f"{temp} = ptrtoint ptr {value} to {target}"], temp
    else:
        return None, None


def lowered(ret, types):
    """A signature as wasm holds it: narrow integers are i32"""
    return NARROW.get(ret, ret), [NARROW.get(t, t) for t in types]


def address_taken(sources, split_files, defined):
    """The functions whose address the IR or the split's data takes"""
    found = set()
    use = re.compile(r"@([\w.$]+)")
    for path in sources:
        for line in Path(path).read_text().split("\n"):
            if line.startswith("define ") or line.startswith("declare "):
                continue
            else:
                # A line of a body or of the data
                pass
            callee = CALL.match(line)
            names = use.findall(line)
            if callee is not None and names and names[0] == callee.group(CALL_CALLEE):
                # The callee of a direct call is no address taken; its arguments may be
                names = names[1:]
            else:
                # No direct call: each name is an address taken
                pass
            found |= {n for n in names if n in defined}
    for path in split_files:
        found |= {n for n in SPLIT_WORD.findall(Path(path).read_text()) if n in defined}
    return found


def dispatcher(name, ret, arg_types, targets, defined):
    """An indirect call of signature (ret, arg_types): each target of another wasm signature called directly, its
    arguments and result converted; any other pointer called as it is"""
    params = ", ".join(f"{t} %a{i}" for i, t in enumerate(arg_types))
    lines = [f"define internal {ret} @{name}(ptr %fn{', ' if arg_types else ''}{params}) {{", "entry:"]
    counter = 0
    for index, target in enumerate(sorted(targets)):
        t_ret, t_ext, t_params = defined[target]
        lines += [f"  %is{index} = icmp eq ptr %fn, @{target}", f"  br i1 %is{index}, label %call{index}, label %next{index}",
                  f"call{index}:"]
        args = []
        for i, (p_type, p_ext) in enumerate(t_params):
            if i < len(arg_types):
                counter += 1
                steps, value = convert(f"%a{i}", arg_types[i], p_type, p_ext, f"%c{counter}")
                if steps is None:
                    return None
                else:
                    # Converted
                    pass
                lines += ["  " + st for st in steps]
            else:
                value = zero(p_type)
            args.append(f"{p_type}{' ' + p_ext if p_ext else ''} {value}")
        ext = f"{t_ext} " if t_ext else ""
        if t_ret == "void" and ret == "void":
            lines += [f"  call void @{target}({', '.join(args)})", "  ret void"]
        elif ret == "void":
            lines += [f"  %r{index} = call {ext}{t_ret} @{target}({', '.join(args)})", "  ret void"]
        elif t_ret == "void":
            lines += [f"  call void @{target}({', '.join(args)})", f"  ret {ret} {zero(ret)}"]
        else:
            lines += [f"  %r{index} = call {ext}{t_ret} @{target}({', '.join(args)})"]
            counter += 1
            steps, value = convert(f"%r{index}", t_ret, ret, t_ext, f"%c{counter}")
            if steps is None:
                return None
            else:
                # Converted
                pass
            lines += ["  " + st for st in steps] + [f"  ret {ret} {value}"]
        lines.append(f"next{index}:")
    call_args = ", ".join(f"{t} %a{i}" for i, t in enumerate(arg_types))
    if ret == "void":
        lines += [f"  call void %fn({call_args})", "  ret void", "}"]
    else:
        lines += [f"  %r = call {ret} %fn({call_args})", f"  ret {ret} %r", "}"]
    return lines


def zero(type_name):
    if type_name == "ptr":
        return "null"
    elif type_name in INTEGER_BITS:
        return "0"
    else:
        return "zeroinitializer"


def main():
    targets, split_files, sources = sys.argv[1].split(","), sys.argv[2].split(","), sys.argv[3:]
    defined, internal_to = {}, {}
    for path in sources:
        for linkage, ret_ext, ret, name, params in DEFINE.findall(Path(path).read_text()):
            if "..." in params:
                continue
            else:
                # A fixed number of parameters
                pass
            defined[name] = (ret, ret_ext.strip(), [parse_param(p) for p in split_args(params)])
            if "internal" in linkage or "private" in linkage:
                # Static: visible to its own file only
                internal_to[name] = str(Path(path).resolve())
            else:
                internal_to.pop(name, None)
    defined.setdefault("atoi", ("i32", "", [("ptr", "")]))
    rewritten = declared = failed = 0
    taken = address_taken(sources, split_files, defined)
    indirect = 0
    for path in targets:
        lines = Path(path).read_text().split("\n")
        out, counter = [], 0
        for line in lines:
            call = CALL.match(line)
            if call is None or call.group(CALL_CALLEE) not in defined:
                out.append(line)
                continue
            else:
                # A direct call of a function defined in the IR
                pass
            indent, result, call_kw, call_attrs, call_ret, name, args_text, tail = call.groups()
            ret, ret_ext, params = defined[name]
            args = [parse_arg(a) for a in split_args(args_text)]
            if call_ret == ret and [a[0] for a in args] == [p[0] for p in params]:
                out.append(line)
                continue
            else:
                # The call's signature differs from the definition's
                pass
            new_args, prelude = [], []
            for index, (param_type, param_ext) in enumerate(params):
                if index < len(args):
                    arg_type, arg_ext, value = args[index]
                    counter += 1
                    steps, converted = convert(value, arg_type, param_type, arg_ext or param_ext, f"%__fc{counter}")
                    if steps is None:
                        break
                    else:
                        # Converted
                        pass
                    prelude += steps
                else:
                    converted = zero(param_type)
                new_args.append(f"{param_type}{' ' + param_ext if param_ext else ''} noundef {converted}")
            else:
                ext = f"{ret_ext} " if ret_ext else ""
                call_text = f"{call_kw}{ext}{ret} @{name}({', '.join(new_args)})"
                counter += 1
                if result is None and ret == "void":
                    after, call_line = [], call_text
                elif result is None:
                    # A value nobody reads, named: an unnamed one would take the next number
                    after, call_line = [], f"%__fc{counter} = {call_text}"
                elif ret == "void" and call_ret in INTEGER_BITS:
                    # The caller reads a value the callee doesn't return: 0
                    after, call_line = [f"{result} = add {call_ret} 0, 0"], call_text
                elif ret == "void" and call_ret == "ptr":
                    after, call_line = [f"{result} = getelementptr i8, ptr null, i32 0"], call_text
                elif ret == "void":
                    failed += 1
                    out.append(line)
                    continue
                else:
                    raw = f"%__fc{counter}"
                    steps, converted = convert(raw, ret, call_ret, ret_ext, result)
                    if steps is None:
                        failed += 1
                        out.append(line)
                        continue
                    else:
                        # Converted
                        pass
                    call_line = f"{raw} = {call_text}" if steps else f"{result} = {call_text}"
                    after = steps
                out += [indent + s for s in prelude] + [indent + call_line] + [indent + s for s in after]
                rewritten += 1
                continue
            failed += 1
            out.append(line)
        # Indirect calls through a dispatcher of their signature
        dispatchers, final = {}, []
        for line in out:
            call = INDIRECT.match(line)
            if call is None:
                final.append(line)
                continue
            else:
                # An indirect call
                pass
            indent, result, call_kw, call_attrs, call_ret, pointer, args_text, tail = call.groups()
            args = [parse_arg(a) for a in split_args(args_text)]
            arg_types = [a[0] for a in args]
            signature = (call_ret, tuple(arg_types))
            if signature not in dispatchers:
                wanted = lowered(call_ret, arg_types)
                here = str(Path(path).resolve())
                others = [t for t in taken if lowered(defined[t][0], [p[0] for p in defined[t][2]]) != wanted
                          and internal_to.get(t, here) == here]
                name = f"__dispatch{len(dispatchers)}"
                dispatchers[signature] = (name, dispatcher(name, call_ret, arg_types, others, defined))
            else:
                # Built for an earlier call of this signature
                pass
            name, body = dispatchers[signature]
            if body is None:
                failed += 1
                final.append(line)
                continue
            else:
                # A dispatcher to call
                pass
            new_args = ", ".join(f"{t} {v}" for t, _, v in args)
            target = f"{call_kw}{call_ret} @{name}(ptr {pointer}{', ' if args else ''}{new_args})"
            final.append(f"{indent}{result + ' = ' if result else ''}{target}")
            indirect += 1
        used = {t for _, body in dispatchers.values() if body for t in re.findall(r"@([\w.$]+)\(", "\n".join(body))}
        text = "\n".join(final)
        for _, body in dispatchers.values():
            if body is not None:
                text += "\n" + "\n".join(body) + "\n"
            else:
                # Not built: its calls were left as they were
                pass
        present = set(re.findall(r"^(?:define|declare) [^@]*@([\w.$]+)\(", text, re.M))
        present |= set(re.findall(r"^@([\w.$]+) = ", text, re.M))
        for target in sorted(used - present):
            if target in defined:
                ret, ret_ext, params = defined[target]
                ext = f"{ret_ext} " if ret_ext else ""
                text += f"\ndeclare {ext}{ret} @{target}({', '.join(t + (' ' + e if e else '') for t, e in params)})\n"
            else:
                # A dispatcher's own name
                pass

        def as_declaration(match):
            nonlocal_declared[0] += 1
            name = match.group(1)
            ret, ret_ext, params = defined[name]
            ext = f"{ret_ext} " if ret_ext else ""
            return f"declare {ext}{ret} @{name}({', '.join(t + (' ' + e if e else '') for t, e in params)})"

        nonlocal_declared = [0]
        text = DECLARE.sub(lambda m: as_declaration(m) if m.group(1) in defined else m.group(0), text)
        text = EXTERNAL.sub(lambda m: as_declaration(m) if m.group(1) in defined else m.group(0), text)
        declared += nonlocal_declared[0]
        Path(path).write_text(text)
    print(f"{rewritten} calls given their callees' signatures, {declared} declarations, {indirect} indirect calls "
          f"through dispatchers ({len(taken)} functions' addresses taken), {failed} left as they were")


if __name__ == "__main__":
    main()
else:
    # Imported: nothing runs
    pass
