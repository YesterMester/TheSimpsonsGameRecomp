"""Extract reviewed health, message and reference-lifetime routines unchanged."""
import argparse
from pathlib import Path
from native_reference import extract


def generate(source, output):
    save = ('__savegprlr_29', '__restgprlr_29')
    routines = [
        (58, 'sub_82957478', 'GameHealthMessage', save + (
            'sub_82937200', 'sub_82957208', 'sub_82937118', 'sub_82956900',
            'sub_82955D00', 'sub_8269ADC0', 'sub_827455E0', 'sub_829573C8',
            'sub_82956530', 'sub_82937650', 'sub_82881400', 'sub_826D90C8',
            'sub_82956768', 'sub_8268F470')),
        (57, 'sub_82937650', 'GameDamageInit', save + ('sub_82937378',)),
        (57, 'sub_82937378', 'GameDamageReset', save + ('sub_827455E0',)),
        (57, 'sub_82937200', 'GameDamageCopy', save + ('sub_823941C0',)),
        (57, 'sub_82937118', 'GameDamageAssign', save + ('sub_823941C0',)),
        (52, 'sub_82881400', 'GameDamageDestroy', ('sub_827455E0',)),
        (58, 'sub_82957208', 'GameCheckDamage', save + (
            'sub_82937020', 'sub_82956C00', 'sub_82A22928')),
        (58, 'sub_829573C8', 'GameCheckHeal', save + ('sub_82937020', 'sub_82956C00')),
        (58, 'sub_82956900', 'GameApplyDamage', ('__savegprlr_27', '__restgprlr_27',
            'sub_8268FAA8', 'sub_82690168', 'sub_827455E0', 'sub_829563B8')),
        (58, 'sub_82956530', 'GameApplyHeal', ('sub_82690168', 'sub_82880798')),
        (58, 'sub_82957018', 'GameIsDead', ()),
        (58, 'sub_82955FC0', 'GameSetHealth', ('sub_8269ADC0', 'sub_82955B08')),
        (58, 'sub_829563B8', 'GameOnDeath', save + (
            'sub_82393DD0', 'sub_826A4518', 'sub_82983CC8', 'sub_82690168', 'sub_827455E0')),
        (58, 'sub_82955D00', 'GameRegisterHealthTick', ('sub_826900B0',)),
        (57, 'sub_82937020', 'GameDamageMask', ()),
        (58, 'sub_82956FF0', 'GameHealthCurrent', ()),
        (58, 'sub_82956FF8', 'GameHealthMaximum', ()),
        (58, 'sub_82956FC0', 'GameHealthEnabled', ()),
        (58, 'sub_82955C28', 'GameLastDamage', ()),
        (52, 'sub_82882C48', 'GameDamageMemberThunk', ()),
        (52, 'sub_828836F8', 'GamePlayerDamageFilter', save + (
            'sub_82892340', 'sub_8269ADC0', 'sub_829F75B0', 'sub_8287DDA0')),
        (11, 'sub_823941C0', 'GameAssignEntityReference', ('sub_827455E0',)),
        (41, 'sub_827455E0', 'GameReleaseEntityReference', ()),
        (52, 'sub_82880798', 'GameDestroyEntityReference', ('sub_827455E0',)),
        (65, '__savegprlr_29', 'GameSave29', ()),
        (65, '__restgprlr_29', 'GameRestore29', ()),
        (65, '__savegprlr_27', 'GameSave27', ()),
        (65, '__restgprlr_27', 'GameRestore27', ()),
        (36, 'sub_8268FAA8', 'GameSendMessage', ('sub_8268F998',)),
        (36, 'sub_8268F998', 'GameDeliverMessage', ('sub_8268F918',)),
    ]
    text = '// Extracted unchanged from the reviewed generated game routines.\n'
    for number, symbol, name, allowed in routines:
        path = source.with_name(f'simpsons_recomp.{number}.cpp')
        digest, body = extract(path, symbol, name, allowed, allow_indirect=True)
        text += f'inline constexpr char k{name}Source[] = "{digest}";\n{body}\n'
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(text, encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    generate(args.source, args.output)
