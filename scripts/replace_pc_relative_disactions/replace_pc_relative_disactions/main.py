import argparse
import re
from typing import Dict, List, Match, Optional

CONNECTIVES_WITHOUT_SEMICOLON = "&|"
CONNECTIVES = CONNECTIVES_WITHOUT_SEMICOLON+r";"

OPERATORS = r"*+\>\<\(\)$"

PCODE_SPECIFIC_OPERATORS = r"\[\]:="

GENERIC_CHARACTER_GROUP_WITH_EQUALS = r"["+CONNECTIVES + OPERATORS+r"\w\s\d=]"

GENERIC_CHARACTER_GROUP_WITHOUT_EQUALS = r"[" + \
    CONNECTIVES + OPERATORS+r"\w\s\d]"

DISPLAY_SECTION = r"(?P<display_section>[][+*\w\s]*)(?:[\s]is[\s])"


DISASSEMBLY_ACTION = r"(?P<action>"+GENERIC_CHARACTER_GROUP_WITH_EQUALS + "+" + \
    "="+GENERIC_CHARACTER_GROUP_WITH_EQUALS + r"+;[\s]*)"

DISASSEMBLY_ACTION_SECTION = r"(?P<action_section>\[" + \
    DISASSEMBLY_ACTION + r"*\])"

SEMANTICS_STATEMENT = "(?P<statement>[\s\w" + \
    PCODE_SPECIFIC_OPERATORS+OPERATORS+CONNECTIVES_WITHOUT_SEMICOLON+"+]*;)"

SEMANTICS_SECTION = r"\s*[{]\s*(?P<semantics>\s" + \
    SEMANTICS_STATEMENT+r"*\s*)\s*[}]\s*"

CONSTRUCTOR_BASE_REGEX = r"(?P<table_name>[\w]*):" + DISPLAY_SECTION + \
    GENERIC_CHARACTER_GROUP_WITH_EQUALS + r"*" + \
    DISASSEMBLY_ACTION_SECTION + r"?" + SEMANTICS_SECTION


class Context:
    def __init__(self) -> None:
        self.program_counter = "$(INST_NEXT_PTR)"


class Environment:
    def __init__(self, program_counter: str, size_hint: str) -> None:
        self.names_to_calculating_expression: Dict[str, str] = {}
        self.program_counter = program_counter
        self.size_hint = size_hint
        self.handle_inst_next_statement("inst_next=inst_next")

    def prepare_statement(self, name: str, exp: str):
        replaced_exp = exp.replace("inst_next", self.program_counter)
        return f"remill_please_dont_use_this_temp_name:{self.size_hint}={name};\nclaim_eq(remill_please_dont_use_this_temp_name,{replaced_exp})"

    def get_priors(self, stat: str) -> List[str]:
        tot = []
        for k, v in self.names_to_calculating_expression.items():
            if k in stat:
                tot.append(v)
        return tot

    def handle_inst_next_statement(self, stat: str):
        if "=" in stat:
            name, exp = stat.split("=", 1)
            if "inst_next" in exp:
                self.names_to_calculating_expression[name] = self.prepare_statement(
                    name, exp)


def build_constructor(env: Environment, constructor: Match[str]) -> Optional[str]:
    semantics_section = constructor.group("semantics")
    cons_start = constructor.start()
    cons_end = constructor.end()
    sem_start = constructor.start("semantics")
    sem_end = constructor.end("semantics")
    new_section = []
    used_priors = False
    for stat in semantics_section.split(";"):
        for prior in env.get_priors(stat):
            new_section.append(prior)
            used_priors = True
        new_section.append(stat)

    if not used_priors:
        return None

    str_sec = ";\n".join(new_section)

    return f"{constructor.string[cons_start:sem_start]}\n{str_sec}\n{constructor.string[sem_end:cons_end]}"


ENDIAN_DEF_REGEX = "define\s*endian\s*=[\w()$]+;"


def main():
    prsr = argparse.ArgumentParser("Disassembly action replacer")
    prsr.add_argument("target_file")
    prsr.add_argument("--pc_def", required=True)
    prsr.add_argument("--inst_next_size_hint", required=True)
    prsr.add_argument("--out", required=True)

    args = prsr.parse_args()
    print(CONSTRUCTOR_BASE_REGEX)
    construct_pat = re.compile(CONSTRUCTOR_BASE_REGEX)

    with open(args.target_file, 'r') as target_f:
        with open(args.pc_def) as pc_def_file:
            pc_def = pc_def_file.read()
            with open(args.out, 'w') as output_f:
                target = target_f.read()
                total_output = ""

                # we know that an endian def has to be the first thing that occurs so go ahead and find that and sub in the preliminaries
                endian_def = re.search(ENDIAN_DEF_REGEX, target)
                total_output += target[0:endian_def.end()]

                # can insert our defs here
                total_output += "\n" + pc_def
                total_output += "\ndefine pcodeop claim_eq;\n"

                last_offset = endian_def.end()
                cont = Context()
                for constructor in construct_pat.finditer(target):
                    total_output += constructor.string[last_offset:constructor.start()]
                    last_offset = constructor.end()
                    env = Environment(cont.program_counter,
                                      args.inst_next_size_hint)
                    act_section = constructor.group("action_section")
                    if act_section:
                        statements = act_section[
                            1: -1].split(";")
                        for stat in statements:
                            env.handle_inst_next_statement(stat)
                    if len(env.names_to_calculating_expression) > 0:
                        maybe_new_cons = build_constructor(env, constructor)
                        if maybe_new_cons:
                            total_output += maybe_new_cons
                        else:
                            total_output += constructor.string[constructor.start(
                            ): constructor.end()]

                total_output += target[last_offset:]
                output_f.write(total_output)


if __name__ == "__main__":
    main()
