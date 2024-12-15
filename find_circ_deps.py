import os
import re
import networkx as nx

def get_files(directory):
    files_list = []
    for root, dirs, files in os.walk(directory):
        # Skip the 'maggi/raylib' directory
        dirs[:] = [d for d in dirs if not os.path.join(root, d).endswith('maggi/raylib')]
        for file in files:
            if file.endswith(('.h', '.c')):
                files_list.append(os.path.join(root, file))
    return files_list

def process_files(files):
    processed_files = {}
    for file in files:
        path, filename = os.path.split(file)
        filename_no_ext, ext = os.path.splitext(filename)
        # Split multiple extensions if present
        extensions = set(ext.lower().split('.'))
        extensions.discard('')  # Remove empty string from the set

        if filename_no_ext not in processed_files:
            processed_files[filename_no_ext] = {'path': path, 'extensions': extensions, 'includes': set()}
        else:
            processed_files[filename_no_ext]['extensions'].update(extensions)

        with open(file, 'r') as f:
            content = f.read()
            includes = re.findall(r'#include\s+"([^"]+)"', content)
            includes_no_ext = [os.path.splitext(os.path.basename(include))[0] for include in includes]
            processed_files[filename_no_ext]['includes'].update(includes_no_ext)

    return processed_files

def construct_dependency_graph(processed_files):
    edges = []
    for filename_no_ext, item in processed_files.items():
        for include in item['includes']:
            if include != filename_no_ext and include in processed_files:
                edges.append((filename_no_ext, include))
    return nx.DiGraph(edges)

if __name__ == "__main__":
    current_directory = os.getcwd()
    files = get_files(current_directory)
    processed_files = process_files(files)
    dependency_graph = construct_dependency_graph(processed_files)

    number_of_cycles = 0
    for cycle in nx.simple_cycles(dependency_graph):
        print(cycle)
        number_of_cycles += 1

    if number_of_cycles > 0:
        print("Found {} cycles".format(number_of_cycles))
        exit(1)
    else:
        print("No cycles found")
    

