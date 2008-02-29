dojo.provide("plugins.reactors.TransformReactor");
dojo.require("plugins.reactors.Reactor");

dojo.declare("plugins.reactors.TransformReactor",
	[ plugins.reactors.Reactor ],
	{
		postCreate: function(){
			this.plugin = 'TransformReactor';
			this.inherited("postCreate", arguments);
		}
	}
);
