# This is a audio tool for windows enabling mixer and soundboard like functionality

AT is a C++ windows native application manipulating audio.

The entirety of its functionality revolves around nodes.

Node is the simplest available unit. A node has an audio input or an audio output or both along with control input and output. There are two plains of connections, one connects audio streams, second one connects control signals. Nodes can be chained from input to output, enabling many to many connections. There are 2 groups of nodes, audio nodes: source nodes, output nodes and processing nodes. Each audio node should therefore support any (limited by a constant, default 4) number of input and output nodes connected. The second group is control nodes. They have only control input and output. In code inheritance hierarchy, the only difference is that the control nodes do not have audio inputs or outputs. Their entire purpose is to exist on the control plane and trigger functions. 

Examples of audio nodes:
  Source nodes:
    Virtual input node - selects an existing windows audio input to listen to and output its audio to the node's output, will be used for example with a microphone
    Playback nodes - example soundboard - plays sound files to its output
  Processing nodes:
    Volume node - for each input node, provides chanel volume control, able to normalize sound volume across all input nodes
    Effect nodes - equalizer, reverb, ...
  Output node - selects a existing windows audio device to playback to, will be used with a virtual audio cable
  Multiple nodes should be able to listen/output to a single windows audio device, if necessary implement a single hidden master node with the devices name, one for each selected windows audio device that gathers all audio coming I/O of that device. 
Examples of control nodes:
  Keyboard input node - on a keyboard input, send a defined trigger to another node to trigger a function. Like on 'M' trigger a specific nodes 'Mute' function.

Create a architecture where:
  Nodes have a defined API interface
    Interface specifies among other things:
      List of available controls - 
        {slider, min_value, max_value}
        {button, toggle on/off}
        {file selector, filters...}
        {audio device selector, filters...}
        {value text input, [list of predefined values]}
        {range text input, min, max, float / int}
        Think of other examples
        The options should also have a preferred style - like "knob, selector switch, slider" on the slider, this will be important for the UI
        Node settings and connections should be serializable for save/load.
        Nodes should have a list of available functions with their names so that the GUI can do things like binding a GUI event to for example "set volume to 0", "lower volume by 10", "increase this parameter"... Functions should have also specified parameter types - int, float, min/max values.
    Nodes can be connected to each other either on a sound stream plane, or control plane.

  Nodes functions can be loaded dynamically from dlls. What everything needs to be specified in the API for it to work correctly?

Take inspiration from existing audio projects on how the audio data is stored and moved to minimize latency.

The nodes should be able to be loaded like plugins, preferably at runtime, at startup is possible

Research existing libraries for audio manipulation, research common pitfalls when processing audio - like clock desync.
All important constants - buffer size... -  should be able to be modified at runtime, implement reload capability. 

The backend should run independently from a GUI managing it. 
GUI application:
  All state of every node, every setting should be saved under a workspace profile file - json is enough.
  This enables of quick loading of different configurations, so the file menu should at least have the options to save, load, create a new profile.

  Multiple profiles should be able to run at the same time, each controlled from its own tab selectable in a top bar of some sort. 

  Main screen of each tab is a visual editing canvas that can be moved with a mouse along with all its nodes and connections displayed.
  Option to create a new node - select from available nodes, right click context menu that on hover displays folders of nodes like they are present on the filesystem.
  Each node is a rectangle with 2 forms - In both forms there is a small topbar with the options to - minimize/maximize the node, disconnect all inputs/outputs, delete. 1st form is a small rectangle with only name, input snap point and output snap point displayed, 2nd form is an expanded form with all the controls displayed - sliders, knobs, input boxes, selectors... 
  Input snap point is a small dot always on the left middle, output on the right middle.
  The user is able to connect input to outputs and vice versa by clicking one, than the other, or dragging. This creates a visual line between these two. The line has a small cross in the middle to delete it and an arrow at the end signaling its direction.

  Do not implement keyboard control node yet, only a simple example timer node, that has one text box configuration that sends a pulse on its data output every [option] ms. The user should be able to switch between audio stream view and data view, both look alike and use the same principles, only with a color difference. When connecting a data output to a node, the receiver node should on select display a context menu with a list of all functions available - where a integer/float value is needed, the number can be supplied with a popup dialog upon selecting the function. 

  Node display guide:
    To keep things simple, nodes are simple rectangles, their control inputs are stacked as rows under each other in a "table". So slider, then under that another control module and so on. Keep the width of the widest element.