import xmltodict
import argparse

# must be the same as in implementation header!
# define SEND_BLOCK_STRING "SEND OPERATION: READING IS ALLOWED!"
# define RECV_BLOCK_STRING "RECEIVE OPERATION: READING IS FORBIDDEN"
SEND_BLOCK_STRING = "SEND OPERATION: READING IS ALLOWED!"
RECV_BLOCK_STRING = "RECEIVE OPERATION: READING IS FORBIDDEN"

#TODO Parsing might break if stacktrace has only length 1 ??

def print_pretty_stackframe(f, use_at=False):
    if use_at:
        print("\tat %s: %s (%s:%s)" % (f['ip'], f['fn'], f['file'], f['line']))
    else:
        print("\tby %s: %s (%s:%s)" % (f['ip'], f['fn'], f['file'], f['line']))


def get_error_count(error_case, counts):
    try:
        return counts[error_case['unique']]
    except KeyError:
        return 1


# return error_originates_from_communication_partition, hide_error
def check_if_error_originates_from_communication_partition(error_case):
    if (error_case['kind'] != 'InvalidRead' and error_case['kind'] != 'InvalidWrite'):
        return False, False
    else:
        #
        mem_region = error_case['auxwhat']
        # originated from original application:
        if not SEND_BLOCK_STRING in mem_region and not RECV_BLOCK_STRING in mem_region:
            return False, False
        # False positife:
        if SEND_BLOCK_STRING in mem_region and error_case['kind'] == 'InvalidRead':
            return False, True

    # found real error
    return True, False


def print_valgrind_like_report(error_case, counts):
    assert (error_case['kind'] == 'InvalidRead' or error_case['kind'] == 'InvalidWrite')
    print("%i errors:" % (get_error_count(error_case, counts)))
    # print(error_case)
    print(error_case['what'])
    print_pretty_stackframe(error_case['stack'][0]['frame'][0], use_at=True)
    for f in error_case['stack'][0]['frame'][1:]:
        print_pretty_stackframe(f)
    print(error_case['auxwhat'])
    print_pretty_stackframe(error_case['stack'][1]['frame'][0], use_at=True)
    for f in error_case['stack'][1]['frame'][1:]:
        print_pretty_stackframe(f)
    assert (len(error_case['stack']) == 2)  # there should be only 2 list of frames
    print("")  # empty line for spacing


def parse_error_counts(raw_input_dict):
    result = {}
    # pairs of count,unique
    print(raw_input_dict["errorcounts"]['pair'])
    if isinstance(raw_input_dict["errorcounts"]['pair'], list):
        for c in raw_input_dict["errorcounts"]['pair']:
            result[c['unique']] = int(c['count'])
    else:
        # no list just one entry
        c = raw_input_dict["errorcounts"]['pair']
        result[c['unique']] = int(c['count'])

    return result


def main():
    parser = argparse.ArgumentParser(
        description='Check Valgrind Report if any errors where introduced through communication partitioning')
    parser.add_argument('valgrind_xml_file', type=argparse.FileType('r'))
    args = parser.parse_args()

    raw_dict = {}
    with args.valgrind_xml_file as file:
        raw_dict = xmltodict.parse(file.read())

    data = raw_dict['valgrindoutput']

    assert (data["tool"] == "memcheck")

    errorcounts = parse_error_counts(data)

    errors_introduced = 0

    if isinstance(data["error"], list):
        case_list = data["error"]
    else:
        # pack it int list
        case_list = [data["error"]]

    for case in case_list:
        error_originates_from_communication_partition, hide_error = check_if_error_originates_from_communication_partition(
            case)
        if error_originates_from_communication_partition:
            print("ERROR INTRODUCED BY COMMUNICATION PARTITIONING!:")
            print_valgrind_like_report(case, errorcounts)
            errors_introduced = errors_introduced + get_error_count(case, errorcounts)
        else:
            if not hide_error:  # due to it being a false positive
                if case['kind'] != 'InvalidRead' and case['kind'] != 'InvalidWrite':
                    print("%i errors of kind %s that can not be introduced by Communication Partition" % (
                        get_error_count(case, errorcounts), case['kind']))
                else:
                    print("NOT introduced by communication partitioning:")
                    print_valgrind_like_report(case, errorcounts)

    print("\nIntroduced %i errors due to Communication partitioning" % errors_introduced)
    if (errors_introduced != 0):
        exit(-1)


if __name__ == "__main__":
    main()
